#include "pf/basic/string.h"
#include "pf/support/helpers.h"
#include "pf/sys/assert.h"
#include "pf/db/query/grammars/grammar.h"
#include "pf/db/connection_interface.h"
#include "pf/db/query/join_clause.h"
#include "pf/db/query/builder.h"

using namespace pf_basic::string;
using namespace pf_basic::type;
using namespace pf_support;
using namespace pf_db::query;

//The builder construct function.
Builder::Builder(ConnectionInterface *connection, grammars::Grammar *grammar) {
  connection_ = connection;
  Assert(connection_);
  if (is_null(connection_)) return;
  grammar_ = is_null(grammar) ? connection->get_query_grammar() : grammar;
  bindings_ = {
    {"select", {}},
    {"join", {}},
    {"where", {}},
    {"having", {}},
    {"order", {}},
    {"union", {}},
  };

  operators_ = {
    "=", "<", ">", "<=", ">=", "<>", "!=", "<=>",
    "like", "like binary", "not like", "between", "ilike",
    "&", "|", "^", "<<", ">>",
    "rlike", "regexp", "not regexp",
    "~", "~*", "!~", "!~*", "similar to",
    "not similar to", "not ilike", "~~*", "!~~*",
  };
   
  distinct_ = false;

  limit_ = -1;

  offset_ = -1;

  union_limit_ = -1;

  union_offset_ = -1;
  
  lock_ = "";
  lock_.type = kVariableTypeInvalid;
}

//The builder destruct function.
Builder::~Builder() {

}

//Clear the all members.
Builder &Builder::clear() {
  bindings_ = {
    {"select", {}},
    {"join", {}},
    {"where", {}},
    {"having", {}},
    {"order", {}},
    {"union", {}},
  };

  distinct_ = false;

  limit_ = -1;

  offset_ = -1;

  union_limit_ = -1;

  union_offset_ = -1;
  
  lock_ = "";
  lock_.type = kVariableTypeInvalid;

  aggregate_.clear();

  columns_.clear();

  joins_.clear();

  wheres_.clear();

  groups_.clear();

  havings_.clear();

  unions_.clear();

  orders_.clear();

  union_orders_.clear();

  if (grammar_) grammar_->set_table_prefix("");
}

//Set the columns to be selected.
Builder &Builder::select(const variable_array_t &columns) {
  columns_ = columns;
  return *this;
}

//Add a new "raw" select expression to the query.
Builder &Builder::select_raw(
    const std::string &expression, const variable_array_t &bindings) {
  add_select({expression});
  if (!bindings.empty())
    add_bindings(bindings, "select");

  return *this;
}

//Add a subselect expression to the query.
Builder &Builder::select_sub(Builder &query, const std::string &as) {
  // Here, we will parse this query into an SQL string and an array of bindings
  // so we can add it to the query builder using the selectRaw method so the 
  // query is included in the real SQL generated by this builder instance.

  std::string sql{""};
  variable_array_t bindings;
  parse_subselect(query, sql, bindings);

  return select_raw("(" + sql + ") as " + grammar_->wrap(as), bindings);
}

//Add a subselect expression to the query.
Builder &Builder::select_sub(
    std::function<void(Builder *)> callback, const std::string &as) {

  //We need safe use the query object with unique_ptr.
  std::unique_ptr<Builder> query;
  Builder *_query = new_query();
  unique_move(Builder, _query, query);

  // If the given query is a Closure, we will execute it while passing in a new
  // query instance to the Closure. This will give the developer a chance to
  // format and work with the query before we cast it to a raw SQL string.
  callback(query.get());

  return select_sub(*query.get(), as);
}

//Parse the sub-select query into SQL and bindings.
void Builder::parse_subselect(
    Builder &query, std::string &sql, variable_array_t &bindings) {
  query.columns_ = {query.columns_[0]};

  sql = query.to_sql();
  bindings = query.get_bindings();
}

//Parse the sub-select query into SQL and bindings.
void Builder::parse_subselect(
    const std::string &query, std::string &sql, variable_array_t &bindings) {
  sql = query;
  bindings = {};
}

//Add a new select column to the query.
Builder &Builder::add_select(const std::vector<std::string> &column) {
  for (const std::string &col : column)
    columns_.emplace_back(col);
  return *this;
}

//Add a join clause to the query.
Builder &Builder::join(const std::string &table, 
                       std::function<void(Builder *)> callback,
                       const std::string &, 
                       const std::string &, 
                       const std::string &type, 
                       bool) {
  std::unique_ptr<JoinClause> _join(new JoinClause(this, type, table));

  // If the first "column" of the join is really a Closure instance the developer
  // is trying to build a join with a complex "on" clause containing more than
  // one condition, so we'll add the join and call a Closure with the query.
  callback(_join.get());

  add_bindings(_join->get_bindings(), "join");
  
  joins_.emplace_back(std::move(_join));


  return *this;
}

//Add a join clause to the query.
Builder &Builder::join(const std::string &table, 
                       const std::string &_first,
                       const std::string &oper, 
                       const std::string &second, 
                       const std::string &type, 
                       bool _where) {

  // If the column is simply a string, we can assume the join simply has a basic 
  // "on" clause with a single condition. So we will just build the join with
  // this simple join clauses attached to it. There is not a join callback.
  std::unique_ptr<JoinClause> _join(new JoinClause(this, type, table));

  if (_where) {
    _join->where(_first, oper, second);
  } else {
    reinterpret_cast<Builder *>(_join.get())->on(_first, oper, second);
  }
  
  add_bindings(_join->get_bindings(), "join");
  
  joins_.emplace_back(std::move(_join));

  return *this;
}

//Add a "cross join" clause to the query.
Builder &Builder::cross_join(const std::string &table, 
                             const std::string &_first, 
                             const std::string &oper, 
                             const std::string &second) {
  if (_first != "")
    return join(table, _first, oper, second, "cross");

  std::unique_ptr<JoinClause> new_join(new JoinClause(this, "cross", table));

  joins_.emplace_back(std::move(new_join));
 
  return *this;
}


void Builder::merge_wheres(std::vector<db_query_array_t> &wheres, 
                           variable_array_t &bindings) {
  for (db_query_array_t &_where : wheres) {
    wheres_.emplace_back(std::move(_where));
  }

  if (bindings_["where"].empty()) 
    bindings_["where"] = {};

  for (variable_t &binding : bindings)
    bindings_["where"].emplace_back(binding);
}

//Add a basic where clause to the query.
Builder &Builder::where(const std::string &column, 
                        const variable_t &oper, 
                        const variable_t &val, 
                        const std::string &boolean) {
  // Here we will make some assumptions about the operator. If only 2 vals are 
  // passed to the method, we will assume that the operator is an equals sign
  // and keep going. Otherwise, we'll require the operator to be passed in.
  bool use_default = (val == "") && ("and" == boolean);
  auto val_oper = prepare_value_and_operator(val, oper, use_default);

  variable_t rval{val_oper[0]};
  variable_t roper{val_oper[1]};

  // If the given operator is not found in the list of valid operators we will
  // assume that the developer is just short-cutting the '=' operators and
  // we will set the operators to '=' and set the vals appropriately.
  if (invalid_operator(roper)) {
    rval = roper;
    roper = "=";
  }

  // If the val is "null", we will just assume the developer wants to add a 
  // where null clause to the query. So, we will allow a short-cut here to
  // that method for convenience so the developer doesn't have to check.
  if (empty(rval))
    return where_null(column, boolean, roper != "=");

  // If the column is making a JSON reference we'll check to see if the val 
  // is a boolean. If it is, we'll add the raw boolean string as an actual
  // val to the query to ensure this is properly handled by the query.
  if (contains(column, {"->"}) && kVariableTypeBool == rval.type) {
    rval = rval == true ? "true" : "false";
    rval.type = static_cast<var_t>(DB_EXPRESSION_TYPE);
  }

  // Now that we are working with just a simple query we can put the elements 
  // in our array and add the query binding to our array of bindings that
  // will be bound to each SQL statements when it is finally executed. 
  std::string type{"basic"};

  db_query_array_t _where;
  _where.items = { //PHP compact can collect the variable with names.
    {"type", type}, {"column", column}, {"operator", roper}, 
    {"value", rval}, {"boolean", boolean}
  };
  wheres_.emplace_back(std::move(_where));

  if (!is_expression(rval))
    add_binding(rval, "where");

  return *this;
}

//Add a basic where clause to the query.
Builder &Builder::where(const std::string &column, 
                        const variable_t &oper, 
                        closure_t val, 
                        const std::string &boolean) {

  // Here we will make some assumptions about the operator. If only 2 vals are 
  // passed to the method, we will assume that the operator is an equals sign
  // and keep going. Otherwise, we'll require the operator to be passed in.
  bool use_default = (oper == "") && ("and" == boolean);
  auto val_oper = prepare_value_and_operator("closure_t", oper, use_default);

  variable_t rval{val_oper[0]};
  variable_t roper{val_oper[1]};

  // If the given operator is not found in the list of valid operators we will
  // assume that the developer is just short-cutting the '=' operators and
  // we will set the operators to '=' and set the vals appropriately.
  if (invalid_operator(roper)) {
    rval = roper;
    roper = "=";
  }

  if (rval == "closure_t")
    return where_sub(column, oper.data, val, boolean);

  // If the val is "null", we will just assume the developer wants to add a 
  // where null clause to the query. So, we will allow a short-cut here to
  // that method for convenience so the developer doesn't have to check.
  if (empty(rval))
    return where_null(column, boolean, roper != "=");

  // If the column is making a JSON reference we'll check to see if the val 
  // is a boolean. If it is, we'll add the raw boolean string as an actual
  // val to the query to ensure this is properly handled by the query.
  if (contains(column, {"->"}) && kVariableTypeBool == rval.type) {
    rval = rval == true ? "true" : "false";
    rval.type = static_cast<var_t>(DB_EXPRESSION_TYPE_S);
  }

  // Now that we are working with just a simple query we can put the elements 
  // in our array and add the query binding to our array of bindings that
  // will be bound to each SQL statements when it is finally executed. 
  std::string type{"basic"};

  db_query_array_t _where;
  _where.items = { //PHP compact can collect the variable with names.
    {"type", type}, {"column", column}, {"operator", roper}, 
    {"value", rval}, {"boolean", boolean}
  };
  wheres_.emplace_back(std::move(_where));

  if (!is_expression(rval))
    add_binding(rval, "where");

  return *this;
}

//Add an array of where clauses to the query.
Builder &Builder::add_array_of_wheres(
    const std::vector<variable_array_t> &columns,
    const std::string &boolean,
    const std::string &method) {
  #define get(n) (vals.size() > (n) ? vals[n].data : "")
  return where_nested([&columns, &method](Builder *query){
    for (auto vals : columns) {
      auto _boolean = "" == get(3) ? "and" : get(3);
      if ("where" == method) {
        query->where(get(0), get(1), get(2), _boolean);
      } else if ("where_column" == method) {
        query->where_column(get(0), get(1), get(2), _boolean);
      }
    }
  }, boolean);
  #undef get
}

//Add an array of where clauses to the query.
Builder &Builder::add_array_of_wheres(variable_set_t &columns,
                                      const std::string &boolean,
                                      const std::string &method) {

  return where_nested([this, &columns, &method](Builder *query){
    for (auto it = columns.begin(); it != columns.end(); ++it) {
      if ("where" == method) {
        query->where(it->first, "=", it->second.data);
      } else if ("where_column") {
        query->where_column(it->first, "=", it->second.data);
      }
    }
  }, boolean);
}

//Prepare the val and operator for a where clause.
variable_array_t Builder::prepare_value_and_operator(const variable_t &val, 
                                                     const variable_t &oper, 
                                                     bool use_default) {
  if (use_default) {
    return {oper, "="};
  } else if (invalid_operator_and_value(oper, val)) {
    AssertEx(false, "Illegal operator and val combination.");
  }
  return {val, oper};
}

//Determine if the given operator and val combination is legal.
bool Builder::invalid_operator_and_value(const variable_t &oper, 
                                         const variable_t &val) {
  return (empty(val) || val == "") && in_array(oper.data, operators_) && 
         !in_array(oper, {"=", "<>", "!="});
}

//Determine if the given operator is supported.
bool Builder::invalid_operator(const std::string &oper) {
  return !in_array(oper, operators_) && 
         !in_array(oper, grammar_->get_operators());
}

//Add a "where" clause comparing two columns to the query.
Builder &Builder::where_column(const std::string &_first, 
                               const std::string &oper, 
                               const std::string &second, 
                               const std::string &boolean) {
  // If the given operator is not found in the list of valid operators we will
  // assume that the developer is just short-cutting the '=' operators and
  // we will set the operators to '=' and set the vals appropriately.
  std::string rsecond{second}, roper{oper};
  if (invalid_operator(oper)) {
    rsecond = oper;
    roper = "=";
  }
  // Finally, we will add this where clause into this array of clauses that we
  // are building for the query. All of them will be compiled via a grammar 
  // once the query is about to be executed and run against the database.
  std::string type{"column"}; //see grammar where_calls_.

  db_query_array_t _where;
  _where.items = { //PHP compact can collect the variable with names.
    {"type", type}, {"first", _first}, {"operator", roper}, 
    {"second", rsecond}, {"boolean", boolean}
  };
  wheres_.emplace_back(std::move(_where));

  return *this;
}

//Add a raw where clause to the query.
Builder &Builder::where_raw(const std::string &sql, 
                            const variable_array_t &bindings, 
                            const std::string &boolean) {
  db_query_array_t _where;
  _where.items = { //PHP compact can collect the variable with names.
    {"type", "raw"}, {"sql", sql}, {"boolean", boolean}
  };
  wheres_.emplace_back(std::move(_where));
 
  add_bindings(bindings, "where");

  return *this;
}

//Add a "where in" clause to the query.
Builder &Builder::where_in(const std::string &column, 
                           const variable_array_t &vals, 
                           const std::string &boolean, 
                           bool isnot) {
  db_query_array_t _where;
  _where.items = { //PHP compact can collect the variable with names.
    {"type", isnot ? "notin" : "in"}, {"column", column}, {"boolean", boolean},
  };

  size_t i{0};
  for (const variable_t &val : vals)
    _where.values[std::to_string(i++)] = val;
  wheres_.emplace_back(std::move(_where));

  // Finally we'll add a binding for each vals unless that val is an expression
  // in which case we will just skip over it since it will be the query as a raw 
  // string and not as a parameterized place-holder to be replaced by the ENV(PDO).
  for (const variable_t &val : vals) {
    if (!is_expression(val))
      add_binding(val, "where");
  }

  return *this;
}

//Add a where in with a sub-select to the query.
Builder &Builder::where_insub(const std::string &column, 
                              closure_t callback, 
                              const std::string &boolean, 
                              bool isnot) {
  auto query = new_query();
  db_query_array_t _where;
  unique_move(Builder, query, _where.query);

  // To create the exists sub-select, we will actually create a query and call the 
  // provided callback with the query so the developer may set any of the query 
  // conditions they want for the in clause, then we'll put it in this array.
  callback(_where.query.get());

  _where.items = {
    {"type", isnot ? "not_insub" : "insub"}, {"column", column}, 
    {"boolean", boolean},
  };

  add_bindings(_where.query->get_bindings(), "where");
  
  wheres_.emplace_back(std::move(_where));

  return *this;
}

//Add an external sub-select to the query.
Builder &Builder::where_in_existing_query(const std::string &column,
                                          Builder &query,
                                          const std::string &boolean,
                                          bool isnot) {
  db_query_array_t _where;
  auto _query = query.new_query();
  unique_move(Builder, _query, _where.query);

  _where.items = {
    {"type", isnot ? "not_insub" : "insub"}, {"column", column}, 
    {"boolean", boolean},
  };
  add_bindings(_where.query->get_bindings(), "where");
  
  wheres_.emplace_back(std::move(_where));
  
  return *this;
}

//Add a "where null" clause to the query.
Builder &Builder::where_null(const std::string &column, 
                             const std::string &boolean, 
                             bool isnot) {
  db_query_array_t _where;
  _where.items = {
    {"type", isnot ? "notnull" : "null"}, {"column", column}, 
    {"boolean", boolean},
  };
  wheres_.emplace_back(std::move(_where));
  return *this;
}

//Add a where between statement to the query.
Builder &Builder::where_between(const std::string &column,
                                const variable_array_t &vals,
                                const std::string &boolean,
                                bool isnot) {
  db_query_array_t _where;
  _where.items = {
    {"type", "between"}, {"column", column}, {"boolean", boolean}, 
    {"not", isnot},
  };

  wheres_.emplace_back(std::move(_where));

  add_bindings(vals, "where");

  return *this;
}

//Add a "where date" statement to the query.
Builder &Builder::where_date(const std::string &column,
                             const std::string &oper,
                             const variable_t &val,
                             const std::string &boolean) {
  bool use_default = (val == "") && ("and" == boolean);
  auto val_oper = prepare_value_and_operator(val, oper, use_default);

  variable_t rval{val_oper[0]};
  std::string roper{val_oper[1].data};
 
  return add_date_based_where("date", column, roper, rval, boolean);
}

//Add a "where day" statement to the query.
Builder &Builder::where_day(const std::string &column, 
                            const std::string &oper, 
                            const variable_t &val, 
                            const std::string &boolean) {
  bool use_default = (val == "") && ("and" == boolean);
  auto val_oper = prepare_value_and_operator(val, oper, use_default);

  variable_t rval{val_oper[0]};
  std::string roper{val_oper[1].data};
 
  return add_date_based_where("day", column, roper, rval, boolean);
}

//Add a "where month" statement to the query.
Builder &Builder::where_month(const std::string &column, 
                              const std::string &oper, 
                              const variable_t &val, 
                              const std::string &boolean) {
  bool use_default = (val == "") && ("and" == boolean);
  auto val_oper = prepare_value_and_operator(val, oper, use_default);

  variable_t rval{val_oper[0]};
  std::string roper{val_oper[1].data};
 
  return add_date_based_where("month", column, roper, rval, boolean);
}

//Add a "where year" statement to the query.
Builder &Builder::where_year(const std::string &column, 
                             const std::string &oper, 
                             const variable_t &val, 
                             const std::string &boolean) {
  bool use_default = (val == "") && ("and" == boolean);
  auto val_oper = prepare_value_and_operator(val, oper, use_default);

  variable_t rval{val_oper[0]};
  std::string roper{val_oper[1].data};
 
  return add_date_based_where("year", column, roper, rval, boolean);
}

//Add another query builder as a nested where to the query builder.
Builder &Builder::add_nested_where_query(std::unique_ptr<Builder> &query,
                                         const std::string &boolean) {
  if (!query->wheres_.empty()) {
    db_query_array_t _where;
    _where.items = {
      {"type", "nested"}, {"boolean", boolean}
    };
    _where.query = std::move(query);
    add_bindings(_where.query->get_bindings(), "where");
    wheres_.emplace_back(std::move(_where));
  }
  return *this;
}

//Add a full sub-select to the query.
Builder &Builder::where_sub(const std::string &column, 
                            const std::string &oper,
                            closure_t callback,
                            const std::string &boolean) {
  // Once we have the query instance we can simply execute it so it can add all 
  // of the sub-select's conditions to itself, and then we can cache it off 
  // in the array of where clauses for the "main" parent query instance.
  db_query_array_t _where;
  auto query = new_query();
  unique_move(Builder, query, _where.query);

  callback(_where.query.get());

  _where.items = {
    {"type", "sub"}, {"column", column}, {"operator", oper}, 
    {"boolean", boolean},
  };

  add_bindings(_where.query->get_bindings(), "where");
  
  wheres_.emplace_back(std::move(_where));

  return *this;
}

//Add a date based (year, month, day, time) statement to the query.
Builder &Builder::add_date_based_where(const std::string &type,
                                       const std::string &column,
                                       const std::string &oper,
                                       const variable_t &val,
                                       const std::string &boolean) {
  db_query_array_t _where;
  _where.items = {
    {"type", type}, {"column", column}, {"operator", oper}, 
    {"boolean", boolean}, {"value", val}
  };
  wheres_.emplace_back(std::move(_where));
  add_binding(val, "where");
  return *this;
}

//Add an exists clause to the query.
Builder &Builder::where_exists(closure_t callback, 
                               const std::string &boolean, 
                               bool isnot) {
  auto query = new_query();

  // Similar to the sub-select clause, we will create a new query instance so
  // the developer may cleanly specify the entire exists query and we will
  // compile the whole thing in the grammar and insert it into the SQL.

  callback(query);

  return add_where_exists_query(query, boolean, isnot);
}

//Add an exists clause to the query.
Builder &Builder::add_where_exists_query(Builder *query, 
                                         const std::string &boolean,
                                         bool isnot) {
  db_query_array_t _where;
  unique_move(Builder, query, _where.query);
  _where.items = {
    {"type", isnot ? "notexists" : "exists"}, {"boolean", boolean},
  };
  wheres_.emplace_back(std::move(_where));
  add_bindings(_where.query->get_bindings(), "where");
  return *this;
}

//Add a "group by" clause to the query.
Builder &Builder::group_by(const variable_array_t &groups) {
  for (const variable_t &group : groups)
    groups_.emplace_back(group);
  return *this;
}

//Add a "having" clause to the query.
Builder &Builder::having(const std::string &column, 
                         const variable_t &oper, 
                         const variable_t &val, 
                         const std::string &boolean) {
  // Here we will make some assumptions about the operator. If only 2 vals are
  // passed to the method, we will assume that the operator is an equals sign 
  // and keep going. Otherwise, we'll require the operator to be passed in.
  bool use_default = (val == "") && ("and" == boolean);
  auto val_oper = prepare_value_and_operator(val, oper, use_default);

  variable_t rval{val_oper[0]};
  variable_t roper{val_oper[1]};

  // If the given operator is not found in the list of valid operators we will
  // assume that the developer is just short-cutting the '=' operators and
  // we will set the operators to '=' and set the vals appropriately.
  if (invalid_operator(roper)) {
    rval = roper;
    roper = "=";
  }
  variable_set_t _having = {
    {"type", "basic"}, {"column", column}, {"operator", roper}, 
    {"value", rval}, {"boolean", boolean},
  };
  havings_.emplace_back(_having);

  if (!is_expression(rval))
    add_binding(rval, "having");

  return *this;
}

//Add a raw having clause to the query.
Builder &Builder::having_raw(const std::string &sql,
                             const variable_array_t &bindings,
                             const std::string &boolean) {
  variable_set_t _having = {
    {"type", "raw"}, {"sql", sql}, {"boolean", boolean},
  };
  havings_.emplace_back(_having);
  if (!bindings.empty())
    add_bindings(bindings, "having");
  return *this;
}

//Add an "order by" clause to the query.
Builder &Builder::order_by(const std::string &column, 
                           const std::string &direction) {
  variable_set_t order = {
    {"column", column}, {"direction", "asc" == direction ? "asc" : "desc"},
  };
  if (unions_.empty()) {
    orders_.emplace_back(order);
  } else {
    union_orders_.emplace_back(order);
  }
  return *this;
}

//Put the query's results in random order.
Builder &Builder::in_random_order(const std::string &seed) {
  return order_byraw(grammar_->compile_random(seed));
}

//Add a raw "order by" clause to the query.
Builder &Builder::order_byraw(const std::string &sql,
                              const variable_array_t &bindings) {
  variable_set_t order = {
    {"type", "raw"}, {"sql", sql},
  };
  if (unions_.empty()) {
    orders_.emplace_back(order);
  } else {
    union_orders_.emplace_back(order);
  }
  add_bindings(bindings, "order");
  return *this;
}

//Set the "offset" val of the query.
Builder &Builder::offset(int32_t val) {
  val = max(0, val);
  if (unions_.empty())
    offset_ = val;
  else
    union_offset_ = val;
  return *this;
}

//Set the "limit" val of the query.
Builder &Builder::limit(int32_t val) {
  if (val >= 0) {
    if (unions_.empty())
      limit_ = val;
    else
      union_limit_ = val;
  }
  return *this;
}

//Get an array orders with all orders for an given column removed.
void Builder::remove_existing_orders_for(const std::string &column, 
                                         std::vector<variable_set_t> &result) {
  result.clear();
  for (variable_set_t &item : orders_) {
    if (item["column"] != column) result.emplace_back(item);
  }
}

//Add a union statement to the query.
Builder &Builder::_union(closure_t callback, bool all) {
  std::unique_ptr<Builder> query(new_query());
  callback(query.get());
  return _union(query, all);
}

//Add a union statement to the query.
Builder &Builder::_union(std::unique_ptr<Builder> &query, bool all) {
  db_query_array_t _where;
  _where.query = std::move(query);
  _where.items = {
    {"all", all},
  };
  add_bindings(_where.query->get_bindings(), "union");
  unions_.emplace_back(std::move(_where));
  return *this;
}

//Get the SQL representation of the query.
std::string Builder::to_sql() {
  return grammar_->compile_select(*this);
}

//Execute a query for a single record by ID.
variable_array_t Builder::find(int32_t id, 
                               const std::vector<std::string> columns) {
  return where("id", "=", id).first(columns);
}

//Get a single column's val from the first result of a query.
variable_t Builder::value(const std::string &column) {
  variable_t _val;
  auto result = first({column});
  if (!result.empty()) _val = result[0];
  return _val;
}

//Execute the query as a "select" statement.
db_fetch_array_t Builder::get(const variable_array_t &columns) {
  auto original = columns_;
  if (original.empty()) {
    columns_ = columns;
  }
  db_fetch_array_t result = run_select();
  columns_ = original;
  return result;
}

//Run the query as a "select" statement against the connection.
db_fetch_array_t Builder::run_select() {
  return connection_->select(to_sql(), get_bindings());
}

//Throw an exception if the query doesn't have an order_by clause.
void Builder::enforce_order_by() {
  if (orders_.empty() && union_orders_.empty())
    AssertEx(false, 
        "You must specify an order_by clause when using this function.");
}

//Determine if any rows exist for the current query.
bool Builder::exists() {
  auto results = 
    connection_->select(grammar_->compile_exists(*this), get_bindings());
  // If the results has rows, we will get the row and see if the exists column is a 
  // boolean true. If there is no results for this query we will return false as 
  // there are no rows for this query at all and we can return that info here.
  if (results.get(0, "exists")) return true;
  return false;
}

//Clean the member by the variable name without "_".
Builder &Builder::clean(const std::string &except) {
  if ("columns" == except) {
    columns_.clear();
  } else if ("distinct" == except) {
    distinct_ = false;
  } else if ("from" == except) {
    from_ = "";
  } else if ("joins" == except) {
    joins_.clear();
  } else if ("wheres" == except) {
    wheres_.clear();
  } else if ("groups" == except) {
    groups_.clear();
  } else if ("havings" == except) {
    havings_.clear();
  } else if ("orders" == except) {
    orders_.clear();
  } else if ("limit" == except) {
    limit_ = -1;
  } else if ("offset" == except) {
    offset_ = -1;
  } else if ("unions" == except) {
    unions_.clear();
  } else if ("union_limit" == except) {
    union_offset_ = -1;
  } else if ("union_offset" == except) {
    union_offset_ = -1;
  } else if ("union_orders" == except) {
    union_orders_.clear();
  } else if ("lock" == except) {
    lock_ = false;
  } else if ("operators" == except) {
    operators_.clear();
  }
  return *this;
}

//Clean the given bindings.
Builder &Builder::clean_bindings(const std::vector<std::string> &except) {
  return tap([this, &except](Builder *query){
    for (const std::string &type : except)
      query->bindings_[type] = {};
  });
}

//Remove all of the expressions from a list of bindings.
variable_array_t Builder::clean_bindings_expression(
    const variable_array_t &bindings) {
  return array_filter<variable_t>(bindings, [](const variable_t &val){
    return !is_expression(val);
  });
}

//Execute an aggregate function on the database.
variable_t Builder::aggregate(const std::string &function, 
                              const variable_array_t &columns) {
  //Need safe delete the query pointer.
  auto _query = new_query();
  std::unique_ptr<Builder> query;
  unique_move(Builder, _query, query);
  auto results = query->clean("columns").
                 clean_bindings({"select"}).
                 set_aggregate(function, columns).
                 get(columns);
  auto _result = results.get(0, "aggregate");
  variable_t result;
  if (_result) result = *_result;
  return result;
}

//Execute a numeric aggregate function on the database.
variable_t Builder::numeric_aggregate(const std::string &function, 
                                      const variable_array_t &columns) {
  auto result = aggregate(function ,columns);
  
  // If there is no result, we can obviously just return 0 here. Next, we will check 
  // if the result is an integer or float. If it is already one of these two data
  // types we can just return the result as-is, otherwise we will convert this.
  if (empty(result)) return 0;

  if (result.type != kVariableTypeString) return result;

  // If the result doesn't contain a decimal place, we will assume it is an int then
  // cast it to one. When it does we will cast it to a float since it needs to be 
  // cast to the expected data type for the developers out of pure convenience.
  return result.data.find(".") != std::string::npos ? 
         result.get<double>() : result.get<int32_t>();
}

//Set the aggregate property without running the query.
Builder &Builder::set_aggregate(const std::string &function, 
                                const variable_array_t &columns) {
  std::string _columns = pf_support::implode(", ", columns);
  aggregate_["function"] = function;
  aggregate_["columns"] = _columns;
  if (groups_.empty()) {
    orders_.clear();
    bindings_["order"] = {};
  }
  return *this;
}

//Insert a new record into the database.
bool Builder::insert(std::vector<variable_set_t> &vals) {
  // Since every insert gets treated like a batch insert, we will make sure the 
  // bindings are structured in a way that is convenient when building these 
  // inserts statements by verifying these elements are actually an array.
  if (vals.empty()) return true;

  // Here, we will sort the insert keys for every record so that each insert is 
  // the results. We will need to also flatten these bindings before running 
  // the query so they are all in one huge, flattened array for execution.

  variable_array_t cbindings;
  for (auto it = vals[0].begin(); it != vals[0].end(); ++it)
    cbindings.emplace_back(it->second);
  
  return connection_->insert(
      grammar_->compile_insert(*this, vals), 
      clean_bindings_expression(cbindings));
}

//Update a record in the database.
int32_t Builder::update(variable_set_t &vals) {
  auto sql = grammar_->compile_update(*this, vals);
  return connection_->update(sql, clean_bindings_expression(
    grammar_->prepare_bindings_forupdate(bindings_, array_values(vals))
  ));
}

//Insert or update a record matching the attributes, and fill it with vals.
bool Builder::update_or_insert(variable_set_t &attributes,
                               variable_set_t &vals) {
  if (!where(attributes).exists()) {
    std::vector<variable_set_t> _vals;
    _vals.emplace_back(array_merge<std::string, variable_t>(attributes, vals));
    return insert(_vals);    
  }
  return false;
}

//Decrement a column's val by a given amount.
int32_t Builder::decrement(const std::string &column, 
                           int32_t amount, 
                           const variable_set_t &extra) {
  auto wrapped = grammar_->wrap(column);

  auto columns = array_merge<std::string, variable_t>({
    {"column", raw("wrapped - " + std::to_string(amount))} 
  }, extra);

  return update(columns);
}

//Delete a record from the database.
int32_t Builder::deleted(const variable_t &id) {
  // If an ID is passed to the method, we will set the where clause to check the 
  // ID to let developers to simply and quickly remove a single row from this
  // database without manually specifying the "where" clauses on the query.
  if (id != "") {
    where(from_ + ".id", "=", id);
  }
  variable_array_t bindings;
  auto it = bindings_.begin();
  if (it != bindings_.end()) bindings = it->second;
  return connection_->deleted(grammar_->compile_delete(*this), bindings);
}

//Run a truncate statement on the table.
void Builder::truncate() {
  auto r = grammar_->compile_truncate(*this);
  for (auto it = r.begin(); it != r.end(); ++it)
    connection_->statement(it->first);
}

//* Create a raw database expression.
variable_t Builder::raw(const variable_t &val) {
  return connection_->raw(val);
}

//Set the bindings on the query builder.
Builder &Builder::set_bindings(variable_array_t &bindings, 
                               const std::string &type) {
  if (bindings_.find(type) == bindings_.end()) {
    std::string msg{"Invalid binding type: "};
    msg += type;
    AssertEx(false, msg.c_str());
    return *this;
  }
  bindings_[type] = bindings;
  return *this;
}

Builder &Builder::add_bindings(const variable_array_t &vals, 
                               const std::string &type) {
  if (bindings_.find(type) == bindings_.end()) {
    std::string msg{"Invalid binding type: "};
    msg += type;
    AssertEx(false, msg.c_str());
    return *this;
  }
  for (const variable_t &item : vals)
    bindings_[type].emplace_back(item);
  return *this;
}

//Add a binding to the query.
Builder &Builder::add_binding(const variable_t &val, 
                              const std::string &type) {
  if (bindings_.find(type) == bindings_.end()) {
    std::string msg{"Invalid binding type: "};
    msg += type;
    AssertEx(false, msg.c_str());
    return *this;
  }
  bindings_[type].emplace_back(val);
  return *this;
}

//Merge an array of bindings into our bindings.
Builder &Builder::merge_bindings(Builder &query) {
  for (auto it = query.bindings_.begin(); it != query.bindings_.end(); ++it) {
    add_bindings(it->second, it->first);
  }
  return *this;
}

variable_array_t Builder::get_bindings() {
  if (bindings_.empty()) return {};
  variable_array_t r;
  for (const std::string &name : DB_BINDING_KEYS) {
    for (const variable_t &value : bindings_[name])
      r.push_back(value);
  }
  return r;
}
