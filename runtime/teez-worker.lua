-- teez-worker DSL runtime

test = {}

local suite_stack = {}
local type_stack = {}
local suite_hooks = {}
local defer_stack = {}
local cleanup_errors = {}
local pending_tests = {}
local only_depth = 0

local function suite_key_from(stack, depth)
    if depth == 0 then
        return ""
    end
    local parts = {}
    for i = 1, depth do
        parts[i] = stack[i]
    end
    return table.concat(parts, " > ")
end

local function suite_key(stack)
    return suite_key_from(stack, #stack)
end

local function copy_stack(stack)
    local copy = {}
    for i, value in ipairs(stack) do
        copy[i] = value
    end
    return copy
end

local function get_hooks(key)
    if not suite_hooks[key] then
        suite_hooks[key] = {
            beforeAll = {},
            afterAll = {},
            beforeEach = {},
            afterEach = {},
            registered = 0,
            completed = 0,
            beforeAll_done = false,
            afterAll_done = false,
        }
    end
    return suite_hooks[key]
end

local function record_cleanup_error(err)
    table.insert(cleanup_errors, tostring(err))
end

local function run_protected(fn)
    local ok, err = pcall(fn)
    if not ok then
        record_cleanup_error(err)
    end
end

local function run_hook_list(hooks)
    for _, fn in ipairs(hooks) do
        run_protected(fn)
    end
end

local function emit_phase(id, phase, state)
    if __teez_emit_phase then
        __teez_emit_phase(id, phase, state)
    end
end

local function suite_scope_id(stack, depth)
    local file = __teez_current_file or "unknown.teez.lua"
    local key = suite_key_from(stack, depth)
    if key == "" then
        return file
    end
    return file .. "::" .. key
end

local function run_hook_list_with_phase(id, phase, hooks)
    if #hooks == 0 then
        return
    end
    emit_phase(id, phase, "start")
    run_hook_list(hooks)
    emit_phase(id, phase, "end")
end

local function current_type()
    for i = #type_stack, 1, -1 do
        if type_stack[i] then
            return type_stack[i]
        end
    end
    return nil
end

local function make_test_descriptor(name, stack, explicit_type)
    local file = __teez_current_file or "unknown.teez.lua"
    return {
        file = file,
        type = filter.resolve_type(file, explicit_type),
        suites = stack,
        name = name,
    }
end

local function make_test_id(name, stack, explicit_type)
    return filter.serialize_id(make_test_descriptor(name, stack, explicit_type))
end

local function should_run_test(name, stack, explicit_type)
    if not __teez_filters then
        return true
    end
    return filter.matches(make_test_descriptor(name, stack, explicit_type), __teez_filters)
end

local function register_test_counts(stack)
    for i = 1, #stack do
        local key = suite_key_from(stack, i)
        local hooks = get_hooks(key)
        hooks.registered = hooks.registered + 1
    end
end

local function complete_skipped_test(stack)
    for i = #stack, 1, -1 do
        local key = suite_key_from(stack, i)
        local hooks = get_hooks(key)
        hooks.completed = hooks.completed + 1
        if hooks.completed == hooks.registered and not hooks.afterAll_done then
            hooks.afterAll_done = true
            run_hook_list(hooks.afterAll)
        end
    end
end

local function register_test(name, fn, mode, retry)
    table.insert(pending_tests, {
        name = name,
        fn = fn or function() end,
        mode = mode,
        retry = retry or 1,
        stack = copy_stack(suite_stack),
        type = current_type(),
        in_only_block = only_depth > 0,
    })
end

local function normalize_each_row(row)
    if type(row) ~= "table" then
        return { row }
    end
    return row
end

local function format_each_title(template, row)
    local index = 0
    return template:gsub("%%s", function()
        index = index + 1
        local value = row[index]
        if value == nil then
            error("test.each title has more %s placeholders than case values")
        end
        return tostring(value)
    end)
end

local function wrap_each_fn(user_fn, row)
    return function(t)
        return user_fn(t, table.unpack(row, 1, #row))
    end
end

local function create_each(mode)
    return function(cases)
        if type(cases) ~= "table" then
            error("test.each requires a table of cases")
        end

        return function(title, user_fn)
            if type(title) ~= "string" then
                error("test.each requires a title template string")
            end
            if type(user_fn) ~= "function" then
                error("test.each requires a test function")
            end

            for _, raw_row in ipairs(cases) do
                local row = normalize_each_row(raw_row)
                local name = format_each_title(title, row)
                register_test(name, wrap_each_fn(user_fn, row), mode)
            end
        end
    end
end

local each_impl = create_each("normal")
test.each = setmetatable({}, {
    __call = function(_, cases)
        return each_impl(cases)
    end,
})
test.each.only = create_each("only")
test.each.skip = create_each("skip")
test.each.todo = create_each("todo")
test.each.fails = create_each("fails")

local function execute_test(test_case)
    local stack = test_case.stack
    if not should_run_test(test_case.name, stack, test_case.type) then
        return
    end

    register_test_counts(stack)
    __teez_active_suite_stack = stack
    local test_id = make_test_id(test_case.name, stack, test_case.type)
    __teez_active_test_id = test_id

    if test_case.mode == "skip" then
        __teez_skip_it(test_id)
        complete_skipped_test(stack)
        return
    end

    if test_case.mode == "todo" then
        __teez_todo_it(test_id)
        complete_skipped_test(stack)
        return
    end

    __teez_run_it(test_id, test_case.fn, test_case.mode, test_case.retry or 1)
end

function __teez_register_defer(fn)
    table.insert(defer_stack, fn)
end

function __teez_before_test()
    defer_stack = {}
    cleanup_errors = {}

    local stack = __teez_active_suite_stack or {}
    local test_id = __teez_active_test_id

    for i = 1, #stack do
        local hooks = get_hooks(suite_key_from(stack, i))
        if not hooks.beforeAll_done then
            hooks.beforeAll_done = true
            run_hook_list_with_phase(suite_scope_id(stack, i), "beforeAll", hooks.beforeAll)
        end
    end

    if test_id then
        for i = 1, #stack do
            run_hook_list_with_phase(test_id, "beforeEach", get_hooks(suite_key_from(stack, i)).beforeEach)
        end
    end
end

function __teez_after_test(finalize)
    for i = #defer_stack, 1, -1 do
        run_protected(defer_stack[i])
    end
    defer_stack = {}

    local stack = __teez_active_suite_stack or {}
    local test_id = __teez_active_test_id

    if test_id then
        for i = #stack, 1, -1 do
            run_hook_list_with_phase(test_id, "afterEach", get_hooks(suite_key_from(stack, i)).afterEach)
        end
    end

    if finalize then
        for i = #stack, 1, -1 do
            local key = suite_key_from(stack, i)
            local hooks = get_hooks(key)
            hooks.completed = hooks.completed + 1
            if hooks.completed == hooks.registered and not hooks.afterAll_done then
                hooks.afterAll_done = true
                run_hook_list_with_phase(suite_scope_id(stack, i), "afterAll", hooks.afterAll)
            end
        end
    end

    return cleanup_errors
end

function __teez_run_all_tests()
    local has_only = false
    for _, test_case in ipairs(pending_tests) do
        if test_case.mode == "only" or test_case.in_only_block then
            has_only = true
            break
        end
    end

    for _, test_case in ipairs(pending_tests) do
        local should_run = not has_only or test_case.mode == "only" or test_case.in_only_block
        if should_run then
            execute_test(test_case)
        end
    end
end

function __teez_collect_all_tests()
    local tests = {}
    local has_only = false
    for _, test_case in ipairs(pending_tests) do
        if test_case.mode == "only" or test_case.in_only_block then
            has_only = true
            break
        end
    end

    for _, test_case in ipairs(pending_tests) do
        local should_run = not has_only or test_case.mode == "only" or test_case.in_only_block
        if should_run and should_run_test(test_case.name, test_case.stack, test_case.type) then
            table.insert(tests, make_test_id(test_case.name, test_case.stack, test_case.type))
        end
    end
    return tests
end

local function describe_impl(name, arg2, arg3)
    local options = {}
    local fn = nil

    if type(arg2) == "table" and type(arg3) == "function" then
        options = arg2
        fn = arg3
    elseif type(arg2) == "function" then
        fn = arg2
    else
        error("invalid describe arguments")
    end

    table.insert(suite_stack, name)
    table.insert(type_stack, options.type)
    fn()
    table.remove(type_stack)
    table.remove(suite_stack)
end

test.describe = setmetatable({
    only = function(name, arg2, arg3)
        only_depth = only_depth + 1
        describe_impl(name, arg2, arg3)
        only_depth = only_depth - 1
    end,
}, {
    __call = function(_, name, arg2, arg3)
        describe_impl(name, arg2, arg3)
    end,
})

function test.beforeAll(fn)
    table.insert(get_hooks(suite_key(suite_stack)).beforeAll, fn)
end

function test.afterAll(fn)
    table.insert(get_hooks(suite_key(suite_stack)).afterAll, fn)
end

function test.beforeEach(fn)
    table.insert(get_hooks(suite_key(suite_stack)).beforeEach, fn)
end

function test.afterEach(fn)
    table.insert(get_hooks(suite_key(suite_stack)).afterEach, fn)
end

local function parse_retry(options)
    if options == nil then
        return 1
    end
    if type(options) ~= "table" then
        error("test options must be a table")
    end
    local retry = options.retry
    if retry == nil then
        return 1
    end
    if type(retry) ~= "number" or retry < 1 then
        error("test option retry must be a positive number")
    end
    return retry
end

local function parse_it_args(name, arg2, arg3, mode)
    local fn = nil
    local retry = 1

    if type(arg2) == "table" and type(arg3) == "function" then
        retry = parse_retry(arg2)
        fn = arg3
    elseif type(arg2) == "function" then
        fn = arg2
    else
        error("invalid test arguments")
    end

    register_test(name, fn, mode, retry)
end

local function register_it(name, arg2, arg3, mode)
    parse_it_args(name, arg2, arg3, mode)
end

test.it = setmetatable({
    each = each_impl,
    only = function(name, arg2, arg3)
        register_it(name, arg2, arg3, "only")
    end,
    skip = function(name, arg2, arg3)
        register_it(name, arg2, arg3, "skip")
    end,
    todo = function(name, arg2, arg3)
        if arg2 == nil and arg3 == nil then
            register_test(name, function() end, "todo", 1)
            return
        end
        register_it(name, arg2, arg3, "todo")
    end,
    fails = function(name, arg2, arg3)
        register_it(name, arg2, arg3, "fails")
    end,
}, {
    __call = function(_, name, arg2, arg3)
        register_it(name, arg2, arg3, "normal")
    end,
})

function test.only(name, arg2, arg3)
    register_it(name, arg2, arg3, "only")
end

function test.skip(name, arg2, arg3)
    register_it(name, arg2, arg3, "skip")
end

function test.todo(name, arg2, arg3)
    if arg2 == nil and arg3 == nil then
        register_test(name, function() end, "todo", 1)
        return
    end
    register_it(name, arg2, arg3, "todo")
end

function test.fails(name, arg2, arg3)
    register_it(name, arg2, arg3, "fails")
end

setmetatable(test, {
    __call = function(_, name, arg2, arg3)
        register_it(name, arg2, arg3, "normal")
    end,
})
