#include "expr.h"
#include "../type/cap.h"
#include "../type/subtype.h"
#include "../type/nominal.h"
#include <assert.h>

/**
 * Make sure the definition of something occurs before its use. This is for
 * both fields and local variable.
 */
static bool def_before_use(ast_t* def, ast_t* use, const char* name)
{
  if((ast_line(def) > ast_line(use)) ||
     ((ast_line(def) == ast_line(use)) &&
      (ast_pos(def) > ast_pos(use))))
  {
    ast_error(use, "declaration of '%s' appears after use", name);
    ast_error(def, "declaration of '%s' appears here", name);
    return false;
  }

  return true;
}

/**
 * Get the nth typedef out of a tuple definition.
 */
static ast_t* tuple_index(ast_t* ast, int index)
{
  assert(ast_id(ast) == TK_TUPLETYPE);

  while(index > 1)
  {
    ast_t* right = ast_childidx(ast, 1);

    if(ast_id(right) != TK_TUPLETYPE)
      return NULL;

    index--;
    ast = right;
  }

  if(index == 0)
    return ast_child(ast);

  ast = ast_childidx(ast, 1);

  if(ast_id(ast) == TK_TUPLETYPE)
    return ast_child(ast);

  return ast;
}

/**
 * If the ast node is a subtype of the named type, return the ast for the type
 * of the ast node. Otherwise, return NULL.
 */
static ast_t* type_builtin(ast_t* ast, const char* name)
{
  ast_t* type = ast_type(ast);
  ast_t* builtin = nominal_builtin(ast, name);
  bool ok = is_subtype(ast, type, builtin);
  ast_free(builtin);

  if(!ok)
    return NULL;

  return type;
}

/**
 * If the ast node is a subtype of Bool, return the ast for the type of the ast
 * node. Otherwise, return NULL.
 */
static ast_t* type_bool(ast_t* ast)
{
  return type_builtin(ast, "Bool");
}

/**
 * If the ast node is a subtype of Integer, return the ast for the type of the
 * ast node. Otherwise, return NULL.
 */
static ast_t* type_int(ast_t* ast)
{
  return type_builtin(ast, "Integer");
}

/**
 * If the ast node is a subtype of Bool or a subtype of Integer, return the ast
 * for the type of the ast node. Otherwise, return NULL.
 */
static ast_t* type_int_or_bool(ast_t* ast)
{
  ast_t* type = type_bool(ast);

  if(type == NULL)
    type = type_int(ast);

  if(type == NULL)
  {
    ast_error(ast, "expected Bool or an integer type");
    return NULL;
  }

  return type;
}

/**
 * If the ast node is a subtype of Arithmetic, return the ast for the type of
 * the ast node. Otherwise, return NULL.
 */
static ast_t* type_arithmetic(ast_t* ast)
{
  return type_builtin(ast, "Arithmetic");
}

/**
 * If one of the two types is a super type of the other, return it. Otherwise,
 * return NULL.
 */
static ast_t* type_super(ast_t* scope, ast_t* l_type, ast_t* r_type)
{
  if((l_type == NULL) || (r_type == NULL))
    return NULL;

  if(is_subtype(scope, l_type, r_type))
    return r_type;

  if(is_subtype(scope, r_type, l_type))
    return l_type;

  return NULL;
}

/**
 * Build a type that is the union of these two types.
 */
static ast_t* type_union(ast_t* ast, ast_t* l_type, ast_t* r_type)
{
  ast_t* super = type_super(ast, l_type, r_type);

  if(super != NULL)
    return super;

  ast_t* type = ast_from(ast, TK_UNIONTYPE);
  ast_add(type, r_type);
  ast_add(type, l_type);

  return type;
}

static ast_t* type_for_fun(ast_t* ast)
{
  assert((ast_id(ast) == TK_NEW) ||
    (ast_id(ast) == TK_BE) ||
    (ast_id(ast) == TK_FUN)
    );

  ast_t* cap = ast_child(ast);
  ast_t* id = ast_sibling(cap);
  ast_t* typeparams = ast_sibling(id);
  ast_t* params = ast_sibling(typeparams);
  ast_t* result = ast_sibling(params);
  ast_t* throws = ast_sibling(result);

  ast_t* fun = ast_from(ast, ast_id(ast));
  ast_add(fun, ast_from(ast, TK_NONE));
  ast_add(fun, throws);
  ast_add(fun, result);

  if(ast_id(params) == TK_PARAMS)
  {
    ast_t* types = ast_from(ast, TK_TYPES);
    ast_t* param = ast_child(params);

    while(param != NULL)
    {
      ast_append(types, ast_childidx(param, 1));
      param = ast_sibling(param);
    }

    ast_add(fun, types);
  } else {
    ast_add(fun, params);
  }

  ast_add(fun, typeparams);
  ast_add(fun, id);
  ast_add(fun, cap);

  return fun;
}

static bool is_lvalue(ast_t* ast)
{
  switch(ast_id(ast))
  {
    case TK_REFERENCE:
    case TK_DOT:
      // an identifier reference is an lvalue. it may still not be valid to
      // assign to it (it could be a method or an SSA that's already set).
      // the same is true for accessing a member with dot notation.
      return true;

    case TK_TUPLE:
    {
      // a tuple is an lvalue if every component expression is an lvalue
      ast_t* child = ast_child(ast);

      while(child != NULL)
      {
        if(!is_lvalue(child))
          return false;

        child = ast_sibling(child);
      }

      return true;
    }

    default: {}
  }

  return false;
}

static bool expr_field(ast_t* ast)
{
  ast_t* type = ast_childidx(ast, 1);
  ast_t* init = ast_sibling(type);

  if((ast_id(type) == TK_NONE) && (ast_id(init) == TK_NONE))
  {
    ast_error(ast, "field/param needs a type or an initialiser");
    return false;
  }

  if(ast_id(type) == TK_NONE)
  {
    // if no declared type, get the type from the initialiser
    ast_settype(ast, ast_type(init));
    return true;
  }

  if(ast_id(init) != TK_NONE)
  {
    // initialiser type must match declared type
    ast_t* init_type = ast_type(init);

    if(!is_subtype(ast, init_type, type))
    {
      ast_error(init,
        "field/param initialiser is not a subtype of the field/param type");
      return false;
    }
  }

  ast_settype(ast, type);
  return true;
}

static bool expr_literal(ast_t* ast, const char* name)
{
  ast_t* type = nominal_builtin(ast, name);

  if(type == NULL)
    return false;

  ast_settype(ast, type);
  return true;
}

static bool expr_this(ast_t* ast)
{
  ast_t* def = ast_enclosing_type(ast);
  assert(def != NULL);
  assert(ast_id(def) != TK_TYPE);

  ast_t* id = ast_child(def);
  ast_t* typeparams = ast_sibling(id);
  const char* name = ast_name(id);

  ast_t* nominal = ast_from(ast, TK_NOMINAL);
  ast_add(nominal, ast_from(ast, TK_NONE)); // ephemerality
  ast_add(nominal, ast_from(ast, cap_for_receiver(ast))); // capability

  if(ast_id(typeparams) == TK_TYPEPARAMS)
  {
    ast_t* typeparam = ast_child(typeparams);
    ast_t* typeargs = ast_from(ast, TK_TYPEARGS);
    ast_add(nominal, typeargs);

    while(typeparam != NULL)
    {
      ast_t* typeparam_id = ast_child(typeparam);
      ast_t* typearg = nominal_type(ast, NULL, ast_name(typeparam_id));
      ast_append(typeargs, typearg);

      typeparam = ast_sibling(typeparam);
    }
  } else {
    ast_add(nominal, ast_from(ast, TK_NONE)); // empty typeargs
  }

  ast_add(nominal, ast_from_string(ast, name));
  ast_add(nominal, ast_from(ast, TK_NONE));
  ast_settype(ast, nominal);

  return true;
}

static bool expr_reference(ast_t* ast)
{
  // everything we reference must be in scope
  const char* name = ast_name(ast_child(ast));
  ast_t* def = ast_get(ast, name);

  if(def == NULL)
  {
    ast_error(ast, "can't find declaration of '%s'", name);
    return false;
  }

  switch(ast_id(def))
  {
    case TK_PACKAGE:
    {
      // only allowed if in a TK_DOT with a type
      if(ast_id(ast_parent(ast)) != TK_DOT)
      {
        ast_error(ast, "a package can only appear as a prefix to a type");
        return false;
      }

      return true;
    }

    case TK_TYPE:
    case TK_CLASS:
    case TK_ACTOR:
    {
      // it's a type name. this may not be a valid type, since it may need
      // type arguments.
      ast_t* id = ast_child(def);
      const char* name = ast_name(id);

      // TODO: this tries to validate the type
      ast_t* type = nominal_type(ast, NULL, name);
      ast_settype(ast, type);
      return true;
    }

    case TK_FVAR:
    case TK_FLET:
    case TK_PARAM:
    {
      if(!def_before_use(def, ast, name))
        return false;

      // get the type of the field/parameter and attach it to our reference
      ast_settype(ast, ast_type(def));
      return true;
    }

    case TK_NEW:
    case TK_BE:
    case TK_FUN:
    {
      // method call on 'this'
      ast_settype(ast, type_for_fun(def));
      return true;
    }

    case TK_IDSEQ:
    {
      // TODO: local, 'as', or 'for'
      if(!def_before_use(def, ast, name))
        return false;

      ast_error(ast, "not implemented (reference local)");
      return false;
    }

    default: {}
  }

  assert(0);
  return false;
}

static bool expr_dot(ast_t* ast)
{
  // TODO: type in package, element in tuple, field or method in object,
  // constructor in type
  // left is a postfix expression, right is an integer or an id
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);
  ast_t* type = ast_type(left);

  switch(ast_id(right))
  {
    case TK_ID:
    {
      if(type == NULL)
      {
        // must be a type in a package
        ast_t* package = ast_get(left, ast_name(left));

        if(package == NULL)
          return false;

        assert(ast_id(package) == TK_PACKAGE);
        const char* package_name = ast_name(left);
        const char* type_name = ast_name(right);
        type = ast_get(package, type_name);

        if(type == NULL)
        {
          ast_error(right, "can't find type '%s' in package '%s'",
            type_name, package_name);
          return false;
        }

        ast_settype(ast, nominal_type(ast, package_name, type_name));
        return true;
      }

      ast_error(ast, "not implemented (dot)");
      return false;

      // TODO: field or method access
      return true;
    }

    case TK_INT:
    {
      // element of a tuple
      if((type == NULL) || (ast_id(type) != TK_TUPLETYPE))
      {
        ast_error(right, "member by position can only be used on a tuple");
        return false;
      }

      type = tuple_index(type, ast_int(right));

      if(type == NULL)
      {
        ast_error(right, "tuple index is out of bounds");
        return false;
      }

      ast_settype(ast, type);
      return true;
    }

    default: {}
  }

  assert(0);
  return false;
}

static bool expr_qualify(ast_t* ast)
{
  // TODO: make sure typeargs are within constraints
  // left is a postfix expression, right is a typeargs
  ast_error(ast, "not implemented (qualify)");
  return false;
}

static bool expr_identity(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);

  ast_t* l_type = ast_type(left);
  ast_t* r_type = ast_type(right);

  if(type_super(ast, l_type, r_type) == NULL)
  {
    ast_error(ast,
      "left and right side must have related types");
    return false;
  }

  return expr_literal(ast, "Bool");
}

static bool expr_compare(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);

  ast_t* l_type = type_arithmetic(left);
  ast_t* r_type = type_arithmetic(right);
  ast_t* type = type_super(ast, l_type, r_type);

  ast_free_unattached(l_type);
  ast_free_unattached(r_type);

  if(type == NULL)
  {
    l_type = ast_type(left);
    r_type = ast_type(right);

    if(!is_subtype(ast, r_type, l_type))
    {
      ast_error(ast, "right side must be a subtype of left side");
      return false;
    }

    // TODO: left side must be Comparable
    // do this in sugar instead?
  }

  ast_settype(ast, nominal_builtin(ast, "Bool"));
  return true;
}

static bool expr_order(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);

  ast_t* l_type = type_arithmetic(left);
  ast_t* r_type = type_arithmetic(right);
  ast_t* type = type_super(ast, l_type, r_type);

  ast_free_unattached(l_type);
  ast_free_unattached(r_type);

  if(type == NULL)
  {
    l_type = ast_type(left);
    r_type = ast_type(right);

    if(!is_subtype(ast, r_type, l_type))
    {
      ast_error(ast, "right side must be a subtype of left side");
      return false;
    }

    // TODO: left side must be Ordered
    // do this in sugar instead?
  }

  ast_settype(ast, nominal_builtin(ast, "Bool"));
  return true;
}

static bool expr_arithmetic(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);

  ast_t* l_type = type_arithmetic(left);
  ast_t* r_type = type_arithmetic(right);
  ast_t* type = type_super(ast, l_type, r_type);

  if(type == NULL)
    ast_error(ast, "left and right side must have related arithmetic types");
  else
    ast_settype(ast, type);

  ast_free_unattached(l_type);
  ast_free_unattached(r_type);

  return (type != NULL);
}

static bool expr_minus(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);
  ast_t* l_type = type_arithmetic(left);
  ast_t* r_type = NULL;
  ast_t* type;

  if(right != NULL)
  {
    r_type = type_arithmetic(right);
    type = type_super(ast, l_type, r_type);

    if(type == NULL)
      ast_error(ast, "left and right side must have related arithmetic types");
  } else {
    type = l_type;

    if(type == NULL)
      ast_error(ast, "must have an arithmetic type");
  }

  if(type != NULL)
    ast_settype(ast, type);

  ast_free_unattached(l_type);
  ast_free_unattached(r_type);

  return (type != NULL);
}

static bool expr_shift(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);

  ast_t* l_type = type_int(left);
  ast_t* r_type = type_int(right);

  if((l_type == NULL) || (r_type == NULL))
  {
    ast_error(ast,
      "left and right side must have integer types");
  } else {
    ast_settype(ast, l_type);
  }

  ast_free_unattached(l_type);
  ast_free_unattached(r_type);

  return (l_type != NULL) && (r_type != NULL);
}

static bool expr_logical(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);

  ast_t* l_type = type_int_or_bool(left);
  ast_t* r_type = type_int_or_bool(right);
  ast_t* type = type_super(ast, l_type, r_type);

  if(type == NULL)
  {
    ast_error(ast,
      "left and right side must have related integer or boolean types");
  } else {
    ast_settype(ast, type);
  }

  ast_free_unattached(l_type);
  ast_free_unattached(r_type);

  return (type != NULL);
}

static bool expr_not(ast_t* ast)
{
  ast_t* child = ast_child(ast);
  ast_t* type = type_int_or_bool(child);

  if(type == NULL)
    return false;

  ast_settype(ast, type);
  return true;
}

static bool expr_tuple(ast_t* ast)
{
  ast_t* child = ast_child(ast);
  ast_t* type;

  if(ast_sibling(child) == NULL)
  {
    type = ast_type(child);
  } else {
    ast_t* tuple = ast_from(ast, TK_TUPLETYPE);
    type = tuple;

    ast_append(tuple, ast_type(child));
    child = ast_sibling(child);

    while(ast_sibling(child) != NULL)
    {
      ast_t* next = ast_from(ast, TK_TUPLETYPE);
      ast_append(tuple, next);
      tuple = next;

      ast_append(tuple, ast_type(child));
      child = ast_sibling(child);
    }

    ast_append(tuple, ast_type(child));
  }

  ast_settype(ast, type);
  return true;
}

static bool expr_call(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* type = ast_type(left);

  switch(ast_id(type))
  {
    case TK_NEW:
    case TK_BE:
    case TK_FUN:
    {
      // first check if the receiver capability is ok
      token_id rcap = cap_for_receiver(ast);
      token_id fcap = cap_for_fun(type);

      if(!is_cap_sub_cap(rcap, fcap))
      {
        ast_error(ast,
          "receiver capability is not a subtype of method capability");
        return false;
      }

      // TODO: use args to decide unbound type parameters
      // TODO: mark enclosing as "may error" if we might error
      // TODO: generate return type for constructors and behaviours
      ast_settype(ast, ast_childidx(type, 4));
      return true;
    }

    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    case TK_NOMINAL:
    case TK_STRUCTURAL:
    case TK_ARROW:
    {
      // TODO: if it's the left side of a TK_ASSIGN, it's update sugar.
      // otherwise, it's apply or create sugar.
      // TODO: is this true? what does a ".method" produce on something
      // other than self?
      ast_error(ast, "not implemented (apply sugar)");
      return false;
    }

    case TK_TUPLETYPE:
    {
      ast_error(ast, "can't call a tuple type");
      return false;
    }

    default: {}
  }

  assert(0);
  return false;
}

static bool expr_if(ast_t* ast)
{
  ast_t* cond = ast_child(ast);
  ast_t* left = ast_sibling(cond);
  ast_t* right = ast_sibling(left);

  if(type_bool(cond) == NULL)
  {
    ast_error(cond, "condition must be a Bool");
    return false;
  }

  ast_t* l_type = ast_type(left);
  ast_t* r_type;

  if(ast_id(right) == TK_NONE)
    r_type = nominal_builtin(ast, "None");
  else
    r_type = ast_type(right);

  ast_t* type = type_union(ast, l_type, r_type);
  ast_settype(ast, type);
  return true;
}

static bool expr_while(ast_t* ast)
{
  ast_t* cond = ast_child(ast);

  if(type_bool(cond) == NULL)
  {
    ast_error(cond, "condition must be a Bool");
    return false;
  }

  ast_settype(ast, nominal_builtin(ast, "None"));
  return true;
}

static bool expr_repeat(ast_t* ast)
{
  ast_t* body = ast_child(ast);
  ast_t* cond = ast_sibling(body);

  if(type_bool(cond) == NULL)
  {
    ast_error(cond, "condition must be a Bool");
    return false;
  }

  ast_settype(ast, nominal_builtin(ast, "None"));
  return true;
}

static bool expr_continue(ast_t* ast)
{
  ast_t* loop = ast_enclosing_loop(ast);

  if(loop == NULL)
  {
    ast_error(ast, "must be in a loop");
    return false;
  }

  if(ast_sibling(ast) != NULL)
  {
    ast_error(ast, "must be the last expression in a sequence");
    ast_error(ast_sibling(ast), "is followed with this expression");
    return false;
  }

  ast_settype(ast, nominal_builtin(ast, "None"));
  return true;
}

static bool expr_return(ast_t* ast)
{
  ast_t* body = ast_child(ast);
  ast_t* type = ast_type(body);
  ast_t* fun = ast_enclosing_method_body(ast);
  bool ok = true;

  if(fun == NULL)
  {
    ast_error(ast, "return must occur in a function or a behaviour body");
    return false;
  }

  if(ast_sibling(ast) != NULL)
  {
    ast_error(ast, "must be the last expression in a sequence");
    ast_error(ast_sibling(ast), "is followed with this expression");
    ok = false;
  }

  switch(ast_id(fun))
  {
    case TK_NEW:
      ast_error(ast, "cannot return in a constructor");
      return false;

    case TK_BE:
    {
      ast_t* none = nominal_builtin(ast, "None");

      if(!is_subtype(ast, type, none))
      {
        ast_error(body, "body of a return in a behaviour must have type None");
        ok = false;
      }

      ast_free(none);
      return ok;
    }

    case TK_FUN:
    {
      ast_t* result = ast_childidx(fun, 4);

      if(ast_id(result) == TK_NONE)
        result = nominal_builtin(ast, "None");

      if(!is_subtype(ast, type, result))
      {
        ast_error(body,
          "body of return doesn't match the function return type");
        ok = false;
      }

      ast_free_unattached(result);
      return ok;
    }

    default: {}
  }

  assert(0);
  return false;
}

static bool expr_assign(ast_t* ast)
{
  ast_t* left = ast_child(ast);
  ast_t* right = ast_sibling(left);
  ast_t* l_type = ast_type(left);
  ast_t* r_type = ast_type(right);

  if(!is_lvalue(left))
  {
    ast_error(ast, "left side must be something that can be assigned to");
    return false;
  }

  // TODO: if left doesn't have a type yet, set it
  if(!is_subtype(ast, r_type, l_type))
  {
    ast_error(ast, "right side must be a subtype of left side");
    return false;
  }

  // TODO: viewpoint adaptation, safe to write, etc
  // TODO: disallow reassignment to SSA variable
  ast_settype(ast, l_type);
  return true;
}

static bool expr_consume(ast_t* ast)
{
  // ast_t* expr = ast_child(ast);
  // ast_t* type = ast_type(expr);

  // TODO:

  ast_error(ast, "not implemented (consume)");
  return false;
}

static bool expr_error(ast_t* ast)
{
  if(ast_sibling(ast) != NULL)
  {
    ast_error(ast, "error must be the last expression in a sequence");
    ast_error(ast_sibling(ast), "error is followed with this expression");
    return false;
  }

  ast_settype(ast, ast_from(ast, TK_ERROR));
  return true;
}

static bool expr_seq(ast_t* ast)
{
  // if any element can error, the whole thing can error
  ast_t* child = ast_child(ast);
  assert(child != NULL);

  ast_t* error = ast_from(ast, TK_ERROR);
  bool can_error = false;
  ast_t* type;

  while(child != NULL)
  {
    type = ast_type(child);
    can_error |= is_subtype(ast, error, type);
    child = ast_sibling(child);
  }

  if(can_error)
    type = type_union(ast, type, error);

  ast_settype(ast, type);
  ast_free_unattached(error);

  return true;
}

static bool expr_fun(ast_t* ast)
{
  ast_t* type = ast_childidx(ast, 4);
  ast_t* can_error = ast_sibling(type);
  ast_t* body = ast_sibling(can_error);

  if(ast_id(body) == TK_NONE)
    return true;

  ast_t* def = ast_enclosing_type(ast);
  bool is_trait = ast_id(def) == TK_TRAIT;

  // if specified, body type must match return type
  ast_t* body_type = ast_type(body);

  if(ast_id(body_type) == TK_ERROR)
  {
    ast_t* last = ast_childlast(body);
    ast_error(type, "function body always results in an error");
    ast_error(last, "function body expression is here");
    return false;
  }

  // check partial functions
  ast_t* error = ast_from(ast, TK_ERROR);
  bool ret = true;

  if(ast_id(can_error) == TK_QUESTION)
  {
    // if a partial function, check that we might actually error
    if(!is_trait && !is_subtype(ast, error, body_type))
    {
      ast_error(can_error, "function body is not partial but the function is");
      ret = false;
    }
  } else {
    // if not a partial function, check that we can't error
    if(is_subtype(ast, error, body_type))
    {
      ast_error(can_error, "function body is partial but the function is not");
      ret = false;
    }
  }

  if(ast_id(type) != TK_NONE)
  {
    // union the result type with ERROR
    if(ast_id(can_error) == TK_QUESTION)
      type = type_union(ast, type, error);

    if(!is_subtype(ast, body_type, type))
    {
      ast_t* last = ast_childlast(body);
      ast_error(type, "function body isn't a subtype of the result type");
      ast_error(last, "function body expression is here");
      ret = false;
    }

    if(!is_trait && !is_eqtype(ast, body_type, type))
    {
      ast_t* last = ast_childlast(body);
      ast_error(type, "function body is more specific than the result type");
      ast_error(last, "function body expression is here");
      ret = false;
    }
  }

  ast_free_unattached(error);
  ast_free_unattached(type);
  return ret;
}

ast_result_t type_expr(ast_t* ast, int verbose)
{
  switch(ast_id(ast))
  {
    case TK_FVAR:
    case TK_FLET:
    case TK_PARAM:
      if(!expr_field(ast))
        return AST_FATAL;
      break;

    case TK_NEW:
      // TODO: check that the object is fully initialised
      if(!expr_fun(ast))
        return AST_FATAL;
      break;

    case TK_BE:
    case TK_FUN:
      if(!expr_fun(ast))
        return AST_FATAL;
      break;

    case TK_SEQ:
      if(!expr_seq(ast))
        return AST_FATAL;
      break;

    case TK_VAR:
    case TK_LET:
      // TODO:
      ast_error(ast, "not implemented (local)");
      return AST_FATAL;

    case TK_CONTINUE:
    case TK_BREAK:
      if(!expr_continue(ast))
        return AST_FATAL;
      break;

    case TK_RETURN:
      if(!expr_return(ast))
        return AST_FATAL;
      break;

    case TK_MULTIPLY:
    case TK_DIVIDE:
    case TK_MOD:
    case TK_PLUS:
      if(!expr_arithmetic(ast))
        return AST_FATAL;
      break;

    case TK_MINUS:
      if(!expr_minus(ast))
        return AST_FATAL;
      break;

    case TK_LSHIFT:
    case TK_RSHIFT:
      if(!expr_shift(ast))
        return AST_FATAL;
      break;

    case TK_LT:
    case TK_LE:
    case TK_GE:
    case TK_GT:
      if(!expr_order(ast))
        return AST_FATAL;
      break;

    case TK_EQ:
    case TK_NE:
      if(!expr_compare(ast))
        return AST_FATAL;
      break;

    case TK_IS:
    case TK_ISNT:
      if(!expr_identity(ast))
        return AST_FATAL;
      break;

    case TK_AND:
    case TK_XOR:
    case TK_OR:
      if(!expr_logical(ast))
        return AST_FATAL;
      break;

    case TK_NOT:
      if(!expr_not(ast))
        return AST_FATAL;
      break;

    case TK_ASSIGN:
      if(!expr_assign(ast))
        return AST_FATAL;
      break;

    case TK_CONSUME:
      if(!expr_consume(ast))
        return AST_FATAL;
      break;

    case TK_DOT:
      if(!expr_dot(ast))
        return AST_FATAL;
      break;

    case TK_QUALIFY:
      if(!expr_qualify(ast))
        return AST_FATAL;
      break;

    case TK_CALL:
      if(!expr_call(ast))
        return AST_FATAL;
      break;

    case TK_IF:
      if(!expr_if(ast))
        return AST_FATAL;
      break;

    case TK_WHILE:
      if(!expr_while(ast))
        return AST_FATAL;
      break;

    case TK_REPEAT:
      if(!expr_repeat(ast))
        return AST_FATAL;
      break;

    case TK_FOR:
      // TODO: transform to a while loop
      ast_error(ast, "not implemented (for)");
      return AST_FATAL;

    case TK_TRY:
      // TODO: type is the union of first and second
      // TODO: check that the first is marked as "may error"
      ast_error(ast, "not implemented (try)");
      return AST_FATAL;

    case TK_TUPLE:
      if(!expr_tuple(ast))
        return AST_FATAL;
      break;

    case TK_ARRAY:
      // TODO: determine our type by looking at every expr in the array
      ast_error(ast, "not implemented (array)");
      return AST_FATAL;

    case TK_OBJECT:
      // TODO: create a structural type for the object
      // TODO: make sure it fulfills any traits it claims to have
      ast_error(ast, "not implemented (object)");
      return AST_FATAL;

    case TK_REFERENCE:
      if(!expr_reference(ast))
        return AST_FATAL;
      break;

    case TK_THIS:
      if(!expr_this(ast))
        return AST_FATAL;
      break;

    case TK_INT:
      if(!expr_literal(ast, "IntLiteral"))
        return AST_FATAL;
      break;

    case TK_FLOAT:
      if(!expr_literal(ast, "FloatLiteral"))
        return AST_FATAL;
      break;

    case TK_STRING:
      if(!expr_literal(ast, "String"))
        return AST_FATAL;
      break;

    case TK_ERROR:
      if(!expr_error(ast))
        return AST_FATAL;
      break;

    default: {}
  }

  return AST_OK;
}