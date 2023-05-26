// This is used in the marked.js extension, 
// which extracts LaTex expressions wrapped in $/$$ and rendered to html.

const options = {
    throwOnError: false
  };
  
function inlineKatex(options) {
  return {
    name: 'inlineKatex',
    level: 'inline',
    start(src) { return src.indexOf('$'); },
    tokenizer(src, tokens) {
      const match = src.match(/^\$+([^$\n]+?)\$+/);
      if (match) {
        return {
          type: 'inlineKatex',
          raw: match[0],
          text: match[1].trim()
        };
      }
    },
    renderer(token) {
      return katex.renderToString(token.text, options);
    }
  };
}

function blockKatex(options) {
  return {
    name: 'blockKatex',
    level: 'block',
    start(src) { return src.indexOf('\n$$'); },
    tokenizer(src, tokens) {
      const match = src.match(/^\$\$+\n([^$]+?)\n\$\$+\n/);
      if (match) {
        return {
          type: 'blockKatex',
          raw: match[0],
          text: match[1].trim()
        };
      }
    },
    renderer(token) {
      return `<p>${katex.renderToString(token.text, options)}</p>`;
    }
  };
}

function markedKatex(options = {}) {
  return {
    extensions: [
      inlineKatex(options),
      blockKatex(options)
    ]
  };
}

marked.use(markedKatex(options));