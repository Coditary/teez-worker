-- Vitest/Jest-compatible expect() matchers for teez-worker

local function assertions()
    local t = __teez_active_assertions
    if t == nil then
        error("expect() must be called inside a running test")
    end
    return t
end

local function strict_equal(actual, expected)
    if type(actual) ~= type(expected) then
        return false
    end
    if type(actual) ~= "table" then
        return actual == expected
    end

    for key, value in pairs(actual) do
        if not strict_equal(value, expected[key]) then
            return false
        end
    end
    for key in pairs(expected) do
        if actual[key] == nil then
            return false
        end
    end
    return true
end

local function get_path_value(root, path)
    local current = root
    for part in string.gmatch(path, "[^%.]+") do
        if type(current) ~= "table" then
            return nil
        end
        current = current[part]
    end
    return current
end

local function call_args_match(call, expected_args)
    for index, expected in ipairs(expected_args) do
        if call[index] ~= expected then
            return false
        end
    end
    return #call >= #expected_args
end

local function mock_was_called_with(mock, expected_args)
    if type(mock.get_calls) ~= "function" then
        error("toHaveBeenCalledWith requires a mock object with get_calls()")
    end
    local calls = mock.get_calls()
    for _, call in ipairs(calls) do
        if call_args_match(call, expected_args) then
            return true
        end
    end
    return false
end

local function is_vitest_falsy(value)
    if value == nil or value == false then
        return true
    end
    if type(value) == "number" then
        return value == 0 or value ~= value
    end
    if type(value) == "string" then
        return value == ""
    end
    return false
end

local matchers = {}

matchers.toBe = function(actual, expected)
    if actual ~= expected then
        error("toBe failed: values are not strictly equal")
    end
end

matchers.toEqual = function(actual, expected)
    if not strict_equal(actual, expected) then
        error("toEqual failed: values are not deeply equal")
    end
end

matchers.toStrictEqual = function(actual, expected)
    if not strict_equal(actual, expected) then
        error("toStrictEqual failed: values are not strictly equal")
    end
end

matchers.toBeTruthy = function(actual)
    if is_vitest_falsy(actual) then
        error("toBeTruthy failed")
    end
end

matchers.toBeFalsy = function(actual)
    if not is_vitest_falsy(actual) then
        error("toBeFalsy failed")
    end
end

matchers.toBeNull = function(actual)
    if actual ~= nil then
        error("toBeNull failed: expected nil")
    end
end

matchers.toBeDefined = function(actual)
    if actual == nil then
        error("toBeDefined failed: value is nil")
    end
end

matchers.toContain = function(actual, item)
    if type(actual) == "string" then
        assertions().assert_contains(actual, tostring(item))
        return
    end
    if type(actual) == "table" then
        assertions().assert_in(actual, item)
        return
    end
    error("toContain requires a string or table")
end

matchers.toMatch = function(actual, pattern)
    if type(actual) ~= "string" then
        error("toMatch requires a string value")
    end
    assertions().assert_match(actual, pattern)
end

matchers.toHaveLength = function(actual, expected_length)
    if type(actual) ~= "string" and type(actual) ~= "table" then
        error("toHaveLength requires a string or table")
    end
    assertions().assert_eq(#actual, expected_length)
end

matchers.toHaveProperty = function(actual, key, expected_value)
    if type(actual) ~= "table" then
        error("toHaveProperty requires a table value")
    end
    if string.find(key, ".", 1, true) then
        local value = get_path_value(actual, key)
        if value == nil then
            error("toHaveProperty failed: path not found (" .. key .. ")")
        end
        if expected_value ~= nil then
            assertions().assert_eq(value, expected_value)
        end
        return
    end

    if expected_value == nil then
        assertions().assert_table_has(actual, key)
    else
        assertions().assert_table_has(actual, key, expected_value)
    end
end

matchers.toBeGreaterThan = function(actual, expected)
    if type(actual) ~= "number" or type(expected) ~= "number" then
        error("toBeGreaterThan requires numeric values")
    end
    if not (actual > expected) then
        error("toBeGreaterThan failed")
    end
end

matchers.toBeLessThan = function(actual, expected)
    if type(actual) ~= "number" or type(expected) ~= "number" then
        error("toBeLessThan requires numeric values")
    end
    if not (actual < expected) then
        error("toBeLessThan failed")
    end
end

matchers.toBeCloseTo = function(actual, expected, digits)
    digits = digits or 2
    local epsilon = 10 ^ (-digits)
    assertions().assert_near(actual, expected, epsilon)
end

matchers.toThrow = function(actual, expected_message)
    if type(actual) ~= "function" then
        error("toThrow requires a function")
    end
    if expected_message == nil then
        assertions().assert_throws(actual)
    else
        assertions().assert_throws(actual, expected_message)
    end
end

matchers.toThrowError = matchers.toThrow

matchers.toHaveBeenCalled = function(actual)
    assertions().assert_called(actual)
end

matchers.toHaveBeenCalledTimes = function(actual, expected_times)
    assertions().assert_called_times(actual, expected_times)
end

matchers.toHaveBeenCalledWith = function(actual, ...)
    local expected_args = { ... }
    if not mock_was_called_with(actual, expected_args) then
        error("toHaveBeenCalledWith failed: mock was not called with expected arguments")
    end
end

matchers.toHaveReturnedWith = function(actual, _)
    if type(actual) ~= "table" or type(actual.get_returns) ~= "function" then
        error("toHaveReturnedWith requires a mock object that tracks return values")
    end
    error("toHaveReturnedWith is not implemented for this mock type")
end

matchers.toMatchSnapshot = function(actual, snapshot_name)
    snapshot_name = snapshot_name or "snapshot"
    assertions().assert_match_snapshot(tostring(actual), snapshot_name)
end

matchers.toMatchInlineSnapshot = function(_, _)
    error("toMatchInlineSnapshot is not supported yet")
end

local function run_matcher(name, actual, negated, ...)
    local matcher = matchers[name]
    if matcher == nil then
        error("unknown matcher: " .. tostring(name))
    end

    local ok, err = pcall(matcher, actual, ...)
    if negated then
        if ok then
            error("expect(...).not." .. name .. " failed")
        end
        return
    end
    if not ok then
        error(err)
    end
end

local function create_expectation(actual, negated)
    return setmetatable({
        actual = actual,
        negated = negated and true or false,
    }, {
        __index = function(self, key)
            if key == "not" then
                return create_expectation(self.actual, not self.negated)
            end
            local matcher = matchers[key]
            if matcher == nil then
                return nil
            end
            return function(...)
                run_matcher(key, self.actual, self.negated, ...)
            end
        end,
    })
end

local expect_api = setmetatable({
    ["not"] = function(_, actual)
        return create_expectation(actual, true)
    end,
}, {
    __call = function(_, actual)
        return create_expectation(actual, false)
    end,
})

expect = expect_api
