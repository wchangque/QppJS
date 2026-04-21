#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/value.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qppjs {

class JSObject;

class JSFunction : public Object {
public:
    ObjectKind object_kind() const override { return ObjectKind::kFunction; }

    const std::optional<std::string>& name() const { return name_; }
    const std::vector<std::string>& params() const { return params_; }
    const std::shared_ptr<std::vector<StmtNode>>& body() const { return body_; }
    const std::shared_ptr<Environment>& closure_env() const { return closure_env_; }
    const std::shared_ptr<JSObject>& prototype_obj() const { return prototype_; }

    void set_name(std::optional<std::string> v) { name_ = std::move(v); }
    void set_params(std::vector<std::string> v) { params_ = std::move(v); }
    void set_body(std::shared_ptr<std::vector<StmtNode>> v) { body_ = std::move(v); }
    void set_closure_env(std::shared_ptr<Environment> v) { closure_env_ = std::move(v); }
    void set_prototype_obj(std::shared_ptr<JSObject> v) { prototype_ = std::move(v); }

private:
    std::optional<std::string> name_;
    std::vector<std::string> params_;
    std::shared_ptr<std::vector<StmtNode>> body_;
    std::shared_ptr<Environment> closure_env_;
    std::shared_ptr<JSObject> prototype_;  // F.prototype (not [[Prototype]] of the function itself)
};

}  // namespace qppjs
