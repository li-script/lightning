// Prism.js language definition for Lightning Script
// Matches editors/vscode/syntaxes/lightning.tmLanguage.json specification

(function (Prism) {
  if (typeof Prism === 'undefined') return;

  Prism.languages.lightning = {
    // 1. Comments
    'comment': [
      {
        pattern: /(^|[^\\])\/\*[\s\S]*?(?:\*\/|$)/,
        lookbehind: true,
        greedy: true
      },
      {
        pattern: /#\[(=*)\[[\s\S]*?\]\1\]/,
        greedy: true
      },
      {
        pattern: /(^|[^\\:])\/\/.*/,
        lookbehind: true,
        greedy: true
      },
      {
        pattern: /(^|[^\[])#(?![\[a-zA-Z0-9_]).*/,
        lookbehind: true,
        greedy: true
      }
    ],

    // 2. Attributes (e.g. [[strict]])
    'attribute': {
      pattern: /\[\[[\s\S]*?\]\]/,
      greedy: true,
      inside: {
        'punctuation': /^\[\[|\]\]$/,
        'attr-name': /\b[a-zA-Z_]\w*\b/
      }
    },

    // 3. Strings
    'raw-string': {
      pattern: /\[(=*)\[[\s\S]*?\]\1\]/,
      greedy: true,
      alias: 'string'
    },
    'template-string': {
      pattern: /`(?:\\[\s\S]|[^\\`])*`/,
      greedy: true,
      inside: {
        'template-punctuation': {
          pattern: /^`|`$/,
          alias: 'string'
        },
        'interpolation': {
          pattern: /\{(?:\\[\s\S]|[^{}\\]|\{(?:[^{}\\]|\{[^}]*\})*\})+\}/,
          inside: {
            'interpolation-punctuation': {
              pattern: /^\{|\}$/,
              alias: 'punctuation'
            },
            rest: null // populated after grammar setup
          }
        },
        'string': /[\s\S]+/
      }
    },
    'string': {
      pattern: /(["'])(?:\\(?:\r\n|[\s\S])|(?!\1)[^\\\r\n])*\1/,
      greedy: true
    },

    // 4. Special Method / Hook declarations (new!, del!, add!, etc.)
    'hook': {
      pattern: /\b(?:new|del|add|sub|mul|at|next|set)!(?=\s*\(|\b)/,
      alias: 'function',
      greedy: true
    },

    // 5. Primitive types
    'type': {
      pattern: /\b(?:number|string|bool|table|array|object|function|i8|i16|i32|i64|u8|u16|u32|u64|f32|f64)\b/,
      alias: 'class-name'
    },

    // 6. Builtin Functions
    'builtin': {
      pattern: /\b(?:print|assert|str|num|int|typeid|typeof|loadstring|eval|struct_copy)\b(?=\s*\(|\b)/,
      alias: 'builtin'
    },

    // 7. Standard Namespaces
    'namespace': {
      pattern: /\b(?:math|table|array|collections|typed|fs|chrono|coroutine|shared|atomic|lock|vec3|weak|reflect|debug|jit)\b(?=::|\.)/,
      alias: 'class-name'
    },

    // 8. Keywords (lexer keywords plus contextual strict words)
    'keyword': /\b(?:as|break|catch|class|const|continue|defer|delete|dyn|else|export|fn|for|if|import|in|is|leave|let|loop|match|return|struct|throw|try|type|while|yield|static|uninit|union|atomic|view|weak)\b/,

    // 9. Literals
    'boolean': /\b(?:true|false)\b/,
    'constant': /\b(?:nil|self)\b/,

    // 10. Function calls
    'function': /\b[a-zA-Z_]\w*(?=\s*\()/,

    // 11. Numbers
    'number': [
      /\b0x[\da-fA-F_]+(?:[iu](?:8|16|32|64))?\b/,
      /\b0b[01_]+(?:[iu](?:8|16|32|64))?\b/,
      /\b\d[\d_]*(?:\.[\d_]+)?(?:[eE][+-]?[\d_]+)?(?:f32|f64|i32|u32|i64|u64|i16|u16|i8|u8)?\b/
    ],

    // 12. Operators
    'operator': /::|->|\.\.|\?\?|&&|\|\||==|!=|<=|>=|[+\-*/%^=<>!&|~]/,

    // 13. Punctuation
    'punctuation': /[{}[\];(),.:]/
  };

  // Wire recursive interpolation inside template strings
  Prism.languages.lightning['template-string'].inside.interpolation.inside.rest = Prism.languages.lightning;

  // Aliases
  Prism.languages.li = Prism.languages.lightning;
})(typeof Prism !== 'undefined' ? Prism : null);
