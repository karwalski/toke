; Tree-sitter highlights for toke
; Supports both legacy profile (80-char) and default syntax (56-char).

; ----------------------------------------------------------------
; Keywords
; ----------------------------------------------------------------

; Declaration keywords — legacy uppercase and default lowercase
(module_decl keyword: _ @keyword)
(import_decl keyword: _ @keyword)
(type_decl keyword: _ @keyword)
(func_decl keyword: _ @keyword)

; Statement keywords
"let" @keyword
"mut" @keyword
"if" @keyword
"el" @keyword
"lp" @keyword
"br" @keyword
"rt" @keyword
"as" @keyword
"arena" @keyword

; Return operator (< used as return)
(return_statement "<" @keyword)

; ----------------------------------------------------------------
; Operators
; ----------------------------------------------------------------

; Arithmetic
"+" @operator
"-" @operator
"*" @operator
"/" @operator

; Comparison
(compare_expression operator: _ @operator)

; Logical — default syntax
"&&" @operator
"||" @operator

; Unary
(unary_expression operator: _ @operator)

; Error propagation
"!" @operator

; ----------------------------------------------------------------
; Punctuation
; ----------------------------------------------------------------

"(" @punctuation.bracket
")" @punctuation.bracket
"{" @punctuation.bracket
"}" @punctuation.bracket
"[" @punctuation.bracket
"]" @punctuation.bracket

";" @punctuation.delimiter
":" @punctuation.delimiter
"." @punctuation.delimiter
"=" @punctuation.delimiter

; Type sigil — default syntax
(sigil_type "$" @punctuation.special)

; Array/map sigil — default syntax
(array_literal "@" @punctuation.special)
(map_literal "@" @punctuation.special)
(array_type_default "@" @punctuation.special)

; ----------------------------------------------------------------
; Types
; ----------------------------------------------------------------

(scalar_type) @type.builtin

(type_identifier) @type
(sigil_type) @type

; ----------------------------------------------------------------
; Functions
; ----------------------------------------------------------------

(func_decl name: (identifier) @function)
(call_expression (postfix_expression "." (identifier) @function.method))
(call_expression (identifier) @function.call)

; .get() index method — default syntax
(index_expression "get" @function.builtin)

; ----------------------------------------------------------------
; Variables and parameters
; ----------------------------------------------------------------

(param name: (identifier) @variable.parameter)
(bind_statement name: (identifier) @variable)
(mut_bind_statement name: (identifier) @variable)
(field_def name: (identifier) @property)
(field_init name: (identifier) @property)

; ----------------------------------------------------------------
; Literals
; ----------------------------------------------------------------

(integer_literal) @number
(float_literal) @number.float
(string_literal) @string
(escape_sequence) @string.escape
(string_interpolation) @string.special
(boolean_literal) @constant.builtin

; ----------------------------------------------------------------
; Module and import paths
; ----------------------------------------------------------------

(module_decl path: (module_path) @module)
(import_decl alias: (identifier) @namespace)
(import_decl path: (module_path) @module)

; ----------------------------------------------------------------
; Match arms
; ----------------------------------------------------------------

(match_arm (type_name) @type)
(match_arm (identifier) @variable)
