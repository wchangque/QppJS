// QppJS test262 harness — 最小实现
// 避免使用 QppJS 不支持的特性: ++, ?:, =>, for...of, 函数对象属性赋值等

var Test262Error = function(message) {
    this.name = "Test262Error";
    this.message = message || "";
};

function _isSameValue(a, b) {
    if (a === b) {
        return (a !== 0) || (1 / a === 1 / b);
    }
    return (a !== a) && (b !== b);
}

function _toString(value) {
    if (value === undefined) {
        return "undefined";
    }
    if (value === null) {
        return "null";
    }
    if (typeof value === "string") {
        return '"' + value + '"';
    }
    return String(value);
}

var assert = {};

assert.sameValue = function(actual, expected, message) {
    if (!_isSameValue(actual, expected)) {
        var msg = message;
        if (!msg) {
            msg = "Expected SameValue(" + _toString(actual) + ", " +
                  _toString(expected) + ") to be true";
        }
        throw new Test262Error(msg);
    }
};

assert.notSameValue = function(actual, unexpected, message) {
    if (_isSameValue(actual, unexpected)) {
        var msg = message;
        if (!msg) {
            msg = "Expected SameValue(" + _toString(actual) + ", " +
                  _toString(unexpected) + ") to be false";
        }
        throw new Test262Error(msg);
    }
};

assert.throws = function(expectedErrorConstructor, func, message) {
    if (typeof func !== "function") {
        throw new Test262Error("assert.throws requires a function argument");
    }
    try {
        func();
    } catch (thrown) {
        if (thrown.constructor !== expectedErrorConstructor) {
            var thrownName = typeof thrown;
            if (thrown.constructor) {
                thrownName = thrown.constructor.name;
            }
            var msg = message;
            if (!msg) {
                msg = "Expected " + expectedErrorConstructor.name +
                      " but got " + thrownName;
            }
            throw new Test262Error(msg);
        }
        return;
    }
    var msg = message;
    if (!msg) {
        msg = "Expected a " + expectedErrorConstructor.name +
              " to be thrown but no exception was thrown";
    }
    throw new Test262Error(msg);
};

assert.compareArray = function(actual, expected, message) {
    if (typeof actual !== "object" || actual === null ||
        typeof expected !== "object" || expected === null) {
        throw new Test262Error("assert.compareArray requires array arguments");
    }
    if (actual.length !== expected.length) {
        var msg = message;
        if (!msg) {
            msg = "Actual and expected have different lengths";
        }
        throw new Test262Error(msg);
    }
    for (var i = 0; i < expected.length; i = i + 1) {
        if (!_isSameValue(actual[i], expected[i])) {
            var msg2 = message;
            if (!msg2) {
                msg2 = "Actual and expected should have the same contents";
            }
            throw new Test262Error(msg2);
        }
    }
};

// assert(value, message) — 也支持直接调用
assert._call = function(mustBeTrue, message) {
    if (mustBeTrue !== true) {
        var msg = message;
        if (!msg) {
            msg = "Expected true but got " + _toString(mustBeTrue);
        }
        throw new Test262Error(msg);
    }
};

var $262 = {
    global: this,
    createRealm: function() {
        throw new Test262Error("$262.createRealm not supported");
    },
    detachArrayBuffer: function(buf) {
        throw new Test262Error("$262.detachArrayBuffer not supported");
    },
    evalScript: function(code) {
        throw new Test262Error("$262.evalScript not supported");
    },
    gc: function() {},
    getGlobal: function(name) {
        return this.global[name];
    },
    setGlobal: function(name, value) {
        this.global[name] = value;
    },
    destroy: function() {},
    IsHTMLDDA: function() {
        return null;
    }
};