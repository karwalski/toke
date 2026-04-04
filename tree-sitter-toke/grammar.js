// Tree-sitter grammar for toke
// Supports both legacy profile (80-char) and default syntax (56-char).

module.exports = grammar({
  name: 'toke',

  extras: $ => [/\s/],

  word: $ => $.identifier,

  conflicts: $ => [
    [$.map_literal, $.array_literal],
    [$.map_type, $.array_type_legacy],
    [$.struct_literal, $.type_name],
  ],

  precedences: $ => [
    [
      'unary',
      'multiplicative',
      'additive',
      'comparative',
      'logical_and',
      'logical_or',
      'match',
    ],
  ],

  rules: {
    source_file: $ => seq(
      $.module_decl,
      repeat($.import_decl),
      repeat(choice($.type_decl, $.const_decl, $.func_decl)),
    ),

    // ----------------------------------------------------------------
    // Module declaration: M=path; or m=path;
    // ----------------------------------------------------------------
    module_decl: $ => seq(
      field('keyword', choice('M', 'm')),
      '=',
      field('path', $.module_path),
      optional(';'),
    ),

    module_path: $ => seq(
      $.identifier,
      repeat(seq('.', $.identifier)),
    ),

    // ----------------------------------------------------------------
    // Import declaration: I=alias:path "url"; or i=alias:path "url";
    // ----------------------------------------------------------------
    import_decl: $ => seq(
      field('keyword', choice('I', 'i')),
      '=',
      field('alias', $.identifier),
      ':',
      field('path', $.module_path),
      optional($.string_literal),
      optional(';'),
    ),

    // ----------------------------------------------------------------
    // Type declaration: T=$name{fields}; or T=Name{fields};
    // ----------------------------------------------------------------
    type_decl: $ => seq(
      field('keyword', choice('T', 't')),
      '=',
      field('name', $.type_name),
      '{',
      $.field_list,
      '}',
      optional(';'),
    ),

    field_list: $ => seq(
      $.field_def,
      repeat(seq(';', $.field_def)),
    ),

    field_def: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $.type_expression),
    ),

    // ----------------------------------------------------------------
    // Constant declaration: name = expr : type;
    // ----------------------------------------------------------------
    const_decl: $ => seq(
      field('name', $.identifier),
      '=',
      field('value', $.literal_expression),
      ':',
      field('type', $.type_expression),
      optional(';'),
    ),

    // ----------------------------------------------------------------
    // Function declaration: F=name(params):ret{body};
    // ----------------------------------------------------------------
    func_decl: $ => seq(
      field('keyword', choice('F', 'f')),
      '=',
      field('name', $.identifier),
      '(',
      optional($.param_list),
      ')',
      ':',
      field('return_type', $.return_spec),
      optional(seq('{', optional($.statement_list), '}')),
      optional(';'),
    ),

    param_list: $ => seq(
      $.param,
      repeat(seq(';', $.param)),
    ),

    param: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $.type_expression),
    ),

    return_spec: $ => seq(
      $.type_expression,
      optional(seq('!', $.type_expression)),
    ),

    // ----------------------------------------------------------------
    // Statements
    // ----------------------------------------------------------------
    statement_list: $ => repeat1($._statement),

    _statement: $ => choice(
      $.bind_statement,
      $.mut_bind_statement,
      $.assign_statement,
      $.return_statement,
      $.if_statement,
      $.loop_statement,
      $.break_statement,
      $.arena_statement,
      $.expression_statement,
    ),

    bind_statement: $ => seq(
      'let',
      field('name', $.identifier),
      '=',
      field('value', $._expression),
      optional(';'),
    ),

    mut_bind_statement: $ => seq(
      'let',
      field('name', $.identifier),
      '=',
      'mut',
      '.',
      field('value', $._expression),
      optional(';'),
    ),

    assign_statement: $ => seq(
      field('name', $.identifier),
      '=',
      field('value', $._expression),
      optional(';'),
    ),

    return_statement: $ => seq(
      choice('<', 'rt'),
      field('value', $._expression),
      optional(';'),
    ),

    break_statement: $ => seq('br', optional(';')),

    if_statement: $ => seq(
      'if',
      '(',
      field('condition', $._expression),
      ')',
      '{',
      optional($.statement_list),
      '}',
      optional(seq('el', '{', optional($.statement_list), '}')),
    ),

    loop_statement: $ => seq(
      'lp',
      '(',
      $.loop_init,
      ';',
      field('condition', $._expression),
      ';',
      $.loop_step,
      ')',
      '{',
      optional($.statement_list),
      '}',
    ),

    loop_init: $ => seq(
      optional('let'),
      field('name', $.identifier),
      '=',
      field('value', $._expression),
    ),

    loop_step: $ => seq(
      field('name', $.identifier),
      '=',
      field('value', $._expression),
    ),

    arena_statement: $ => seq(
      '{',
      'arena',
      optional($.statement_list),
      '}',
    ),

    expression_statement: $ => seq(
      $._expression,
      optional(';'),
    ),

    // ----------------------------------------------------------------
    // Expressions — precedence from lowest to highest
    // ----------------------------------------------------------------
    _expression: $ => choice(
      $.match_expression,
      $.logical_or_expression,
      $.logical_and_expression,
      $.compare_expression,
      $.add_expression,
      $.mul_expression,
      $.unary_expression,
      $.cast_expression,
      $.propagate_expression,
      $.call_expression,
      $.index_expression,
      $.postfix_expression,
      $._primary_expression,
    ),

    // match: expr |{ arms }
    match_expression: $ => prec('match', seq(
      $._expression,
      '|',
      '{',
      $.match_arm_list,
      '}',
    )),

    match_arm_list: $ => seq(
      $.match_arm,
      repeat(seq(';', $.match_arm)),
    ),

    match_arm: $ => seq(
      $.type_name,
      ':',
      $.identifier,
      $._expression,
    ),

    // logical or: expr || expr (default syntax)
    logical_or_expression: $ => prec.left('logical_or', seq(
      $._expression,
      '||',
      $._expression,
    )),

    // logical and: expr && expr (default syntax)
    logical_and_expression: $ => prec.left('logical_and', seq(
      $._expression,
      '&&',
      $._expression,
    )),

    // comparison: expr (< | > | =) expr
    compare_expression: $ => prec.left('comparative', seq(
      $._expression,
      field('operator', choice('<', '>', '=')),
      $._expression,
    )),

    // addition: expr (+ | -) expr
    add_expression: $ => prec.left('additive', seq(
      $._expression,
      field('operator', choice('+', '-')),
      $._expression,
    )),

    // multiplication: expr (* | /) expr
    mul_expression: $ => prec.left('multiplicative', seq(
      $._expression,
      field('operator', choice('*', '/')),
      $._expression,
    )),

    // unary: -expr | !expr
    unary_expression: $ => prec('unary', seq(
      field('operator', choice('-', '!')),
      $._expression,
    )),

    // cast: expr as type
    cast_expression: $ => seq(
      $._expression,
      'as',
      $.type_expression,
    ),

    // error propagation: expr ! type
    propagate_expression: $ => seq(
      $.call_expression,
      '!',
      $.type_expression,
    ),

    // function call: expr(args)
    call_expression: $ => seq(
      choice($.postfix_expression, $._primary_expression),
      '(',
      optional($.arg_list),
      ')',
    ),

    // index expression: expr.get(expr) — default syntax for variable indexing
    index_expression: $ => seq(
      $._expression,
      '.',
      'get',
      '(',
      $._expression,
      ')',
    ),

    // postfix / field access: expr.ident
    postfix_expression: $ => prec.left(seq(
      $._expression,
      '.',
      $.identifier,
    )),

    arg_list: $ => seq(
      $._expression,
      repeat(seq(';', $._expression)),
    ),

    _primary_expression: $ => choice(
      $.identifier,
      $.literal_expression,
      $.parenthesized_expression,
      $.struct_literal,
      $.array_literal,
      $.map_literal,
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    // ----------------------------------------------------------------
    // Struct, array, and map literals
    // ----------------------------------------------------------------

    // Struct literal: TypeName{field:expr; ...}
    struct_literal: $ => seq(
      $.type_name,
      '{',
      $.field_init,
      repeat(seq(';', $.field_init)),
      '}',
    ),

    field_init: $ => seq(
      field('name', $.identifier),
      ':',
      field('value', $._expression),
    ),

    // Array literal — legacy: [expr; ...] — default: @(expr; ...)
    array_literal: $ => choice(
      seq('[', optional(seq($._expression, repeat(seq(';', $._expression)))), ']'),
      seq('@', '(', optional(seq($._expression, repeat(seq(';', $._expression)))), ')'),
    ),

    // Map literal — legacy: [key:val; ...] — default: @(key:val; ...)
    map_literal: $ => choice(
      seq('[', $.map_entry, repeat(seq(';', $.map_entry)), ']'),
      seq('@', '(', $.map_entry, repeat(seq(';', $.map_entry)), ')'),
    ),

    map_entry: $ => seq(
      field('key', $._expression),
      ':',
      field('value', $._expression),
    ),

    // ----------------------------------------------------------------
    // Type expressions
    // ----------------------------------------------------------------
    type_expression: $ => choice(
      $.pointer_type,
      $.map_type,
      $.array_type_legacy,
      $.array_type_default,
      $.func_type,
      $.scalar_type,
      $.type_name,
    ),

    pointer_type: $ => seq('*', $.type_expression),

    // Map type — legacy: [KeyType:ValType] — default: @(KeyType:ValType)
    map_type: $ => choice(
      seq('[', $.type_expression, ':', $.type_expression, ']'),
      seq('@', '(', $.type_expression, ':', $.type_expression, ')'),
    ),

    // Array type — legacy: [Type]
    array_type_legacy: $ => seq('[', $.type_expression, ']'),

    // Array type — default: @type
    array_type_default: $ => seq('@', $.type_expression),

    // Function type: (ParamType; ...):RetType
    func_type: $ => seq(
      '(',
      $.type_expression,
      repeat(seq(';', $.type_expression)),
      ')',
      ':',
      $.type_expression,
    ),

    scalar_type: $ => choice(
      'u8', 'u16', 'u32', 'u64',
      'i8', 'i16', 'i32', 'i64',
      'f32', 'f64',
      'bool',
      'void',
    ),

    // Type name — legacy: UpperCaseIdent — default: $lowercase
    type_name: $ => choice(
      $.type_identifier,
      $.sigil_type,
    ),

    // $typename (default syntax)
    sigil_type: $ => seq('$', $.identifier),

    // UpperCase type identifier (legacy profile)
    type_identifier: $ => /[A-Z][a-zA-Z0-9_]*/,

    // ----------------------------------------------------------------
    // Literals
    // ----------------------------------------------------------------
    literal_expression: $ => choice(
      $.integer_literal,
      $.float_literal,
      $.string_literal,
      $.boolean_literal,
    ),

    integer_literal: $ => choice(
      /[0-9]+/,
      /0x[0-9a-fA-F]+/,
      /0b[01]+/,
    ),

    float_literal: $ => /[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?/,

    string_literal: $ => seq(
      '"',
      repeat(choice(
        /[^"\\]+/,
        $.escape_sequence,
        $.string_interpolation,
      )),
      '"',
    ),

    escape_sequence: $ => /\\[nrt\\"]/,

    string_interpolation: $ => seq('\\', '(', $._expression, ')'),

    boolean_literal: $ => choice('true', 'false'),

    // ----------------------------------------------------------------
    // Identifiers
    // ----------------------------------------------------------------
    identifier: $ => /[a-z_][a-zA-Z0-9_]*/,

  },
});
