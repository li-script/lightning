// Lightning Script Web REPL Application

(function () {
  'use strict';

  // --- Curated Examples ---
  const EXAMPLES = {
    welcome: `// Welcome to Lightning Script!
print("Hello from the Lightning Script playground!");
let version = "0.1";
print(\`Running Lightning v{version} via WebAssembly\`);

// Quick math and variables:
let radius = 5;
let area = 3.14159 * radius * radius;
print(\`Circle with radius {radius} has area {area}\`);
`,

    fibonacci: `// Fibonacci calculation
fn fib(n) {
  if n <= 1 { return n; }
  return fib(n - 1) + fib(n - 2);
}

print("Calculating Fibonacci sequence:");
for i in 0..12 {
  print(\`fib({i}) = {fib(i)}\`);
}
`,

    tables: `// Dictionaries / Tables and Arrays
let user = {
  name: "Alice",
  role: "Engineer",
  languages: ["Rust", "C++", "Lightning"]
};

print("User name:", user.name);
print("Role:", user.role);

for idx, lang in user.languages {
  print(\`Language #{idx + 1}: {lang}\`);
}
`,

    structs: `// Structs (value semantics) and Classes (reference semantics)
struct Point {
  x: number = 0
  y: number = 0

  new!(x: number, y: number) {
    self.x = x
    self.y = y
  }

  get length_sq() -> number {
    self.x * self.x + self.y * self.y
  }

  add!(other: Point) -> Point {
    let res = struct_copy(self)
    res.x += other.x
    res.y += other.y
    res
  }
}

let p1 = Point(3, 4)
let p2 = Point(1, 2)
let sum = p1 + p2

print("Point 1:", p1.x, p1.y)
print("Point 1 Length Squared:", p1.length_sq)
print("Point 2:", p2.x, p2.y)
print("Sum Point:", sum.x, sum.y)
`,

    closures: `// Closures & Higher-Order Functions
fn make_counter(start) {
  let count = start;
  return || {
    count += 1;
    return count;
  };
}

let counter_a = make_counter(0);
let counter_b = make_counter(100);

print("Counter A:", counter_a());
print("Counter A:", counter_a());
print("Counter B:", counter_b());
print("Counter A:", counter_a());
`,

    loops: `// Range loops and conditionals
print("Checking even numbers from 0 to 10:");
for i in 0..11 {
  if i % 2 == 0 {
    print(\`{i} is even\`);
  }
}
`
  };

  // --- DOM Elements ---
  const editor = document.getElementById('code-editor');
  const highlightCode = document.getElementById('highlight-code');
  const editorHighlight = document.getElementById('editor-highlight');
  const lineNumbers = document.getElementById('line-numbers');
  const outputLog = document.getElementById('output-log');
  const outputContainer = document.getElementById('output-container');
  const replInput = document.getElementById('repl-input');
  const btnRun = document.getElementById('btn-run');
  const btnClear = document.getElementById('btn-clear');
  const btnReset = document.getElementById('btn-reset');
  const btnReplSend = document.getElementById('btn-repl-send');
  const replForm = document.getElementById('repl-form');
  const btnCopyCode = document.getElementById('btn-copy-code');
  const exampleSelect = document.getElementById('example-select');
  const statusBadge = document.getElementById('status-badge');
  const editorStatus = document.getElementById('editor-status');
  const resizer = document.getElementById('resizer');
  const editorPane = document.getElementById('editor-pane');
  const workspace = document.getElementById('workspace');

  let isRuntimeReady = false;
  let runtimeUnavailable = false;
  let replHistory = [];
  let historyIndex = -1;

  // --- ANSI to HTML Converter ---
  function escapeHtml(str) {
    return str
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
  }

  function ansiToHtml(text) {
    const ansiMap = {
      '1;31': 'ansi-red',
      '1;32': 'ansi-green',
      '1;33': 'ansi-yellow',
      '1;34': 'ansi-blue',
      '1;35': 'ansi-purple',
      '1;36': 'ansi-cyan',
      '1;37': 'ansi-bright',
      '31': 'ansi-red',
      '32': 'ansi-green',
      '33': 'ansi-yellow',
      '34': 'ansi-blue',
      '35': 'ansi-purple',
      '36': 'ansi-cyan',
      '37': 'ansi-bright',
    };

    let html = '';
    let spanOpen = 0;
    const parts = text.split(/(\x1B\[[0-9;]*m)/g);

    for (const part of parts) {
      if (part.startsWith('\x1B[')) {
        const code = part.slice(2, -1);
        if (code === '0' || code === '') {
          while (spanOpen > 0) {
            html += '</span>';
            spanOpen--;
          }
        } else if (ansiMap[code]) {
          html += `<span class="${ansiMap[code]}">`;
          spanOpen++;
        }
      } else {
        html += escapeHtml(part);
      }
    }

    while (spanOpen > 0) {
      html += '</span>';
      spanOpen--;
    }

    return html;
  }

  function appendOutput(rawText, type = 'stdout') {
    const formatted = ansiToHtml(rawText);
    const className = type === 'stderr' ? 'output-error' : type === 'echo' ? 'repl-echo' : '';
    outputLog.insertAdjacentHTML('beforeend', className ? `<span class="${className}">${formatted}</span>` : formatted);
    outputContainer.scrollTop = outputContainer.scrollHeight;
  }

  function setStatus(state, text) {
    statusBadge.className = `status-indicator ${state}`;
    statusBadge.innerHTML = `<span class="status-dot" aria-hidden="true"></span><span class="status-text">${text}</span>`;
  }

  // --- Emscripten Module Setup ---
  window.Module = window.Module || {};
  window.Module.print = function (text) {
    appendOutput(text + '\n');
  };
  window.Module.printErr = function (text) {
    appendOutput(text + '\n', 'stderr');
  };
  window.Module.onRuntimeUnavailable = function () {
    if (isRuntimeReady || runtimeUnavailable) return;
    runtimeUnavailable = true;
    setStatus('error', 'Runtime unavailable');
    appendOutput('The WebAssembly runtime could not be loaded. Serve li.js and li.wasm with this page to run code.\n', 'stderr');
  };
  window.Module.onRuntimeInitialized = function () {
    isRuntimeReady = true;
    runtimeUnavailable = false;
    setStatus('ready', 'Ready');

    editor.disabled = false;
    btnRun.disabled = false;
    replInput.disabled = false;
    btnReplSend.disabled = false;

    appendOutput('Runtime initialized. Run the editor or evaluate an expression below.\n\n');
  };

  // --- Script Execution ---
  function runScript(source) {
    if (!isRuntimeReady || !window.Module.ccall) {
      appendOutput(runtimeUnavailable ? 'Runtime unavailable.\n' : 'Runtime is still loading.\n', 'stderr');
      return;
    }

    setStatus('running', 'Running');

    try {
      window.Module.ccall('runscript', null, ['string'], [source]);
      setStatus('ready', 'Ready');
    } catch (err) {
      appendOutput(`Runtime exception: ${err.message || err}\n`, 'stderr');
      setStatus('error', 'Runtime unavailable');
    }
  }

  function handleEditorRun() {
    const code = editor.value.trim();
    if (!code) return;
    appendOutput(`Running script (${editor.value.split('\n').length} lines)\n`, 'echo');
    runScript(editor.value);
  }

  function handleReplSubmit() {
    const input = replInput.value.trim();
    if (!input) return;

    appendOutput(`› ${input}\n`, 'echo');
    replHistory.push(input);
    historyIndex = replHistory.length;
    replInput.value = '';

    runScript(input);
  }

  function resetVM() {
    if (!isRuntimeReady || !window.Module.ccall) return;
    try {
      window.Module.ccall('reset_vm', null, [], []);
      appendOutput('Runtime state reset.\n', 'echo');
    } catch (err) {
      appendOutput(`Failed to reset runtime: ${err.message || err}\n`, 'stderr');
    }
  }

  // --- Editor Line Numbers, Syntax Highlighting & Navigation ---
  function updateEditor() {
    const val = editor.value;
    const lines = val.split('\n').length;
    let numbers = '';
    for (let i = 1; i <= lines; i++) {
      numbers += i + '\n';
    }
    lineNumbers.textContent = numbers;

    // Prism is the single source of syntax highlighting.
    if (window.Prism && Prism.languages.lightning) {
      highlightCode.innerHTML = Prism.highlight(val, Prism.languages.lightning, 'lightning');
    } else {
      highlightCode.textContent = val;
    }

    // Update cursor line / col
    const pos = editor.selectionStart;
    const currentLine = val.substring(0, pos).split('\n').length;
    const currentCol = pos - val.lastIndexOf('\n', pos - 1);
    editorStatus.textContent = `Line ${currentLine}, Col ${currentCol}`;
  }

  function syncScroll() {
    lineNumbers.scrollTop = editor.scrollTop;
    editorHighlight.scrollTop = editor.scrollTop;
    editorHighlight.scrollLeft = editor.scrollLeft;
  }

  // --- Event Listeners ---
  btnRun.addEventListener('click', handleEditorRun);
  btnClear.addEventListener('click', () => { outputLog.innerHTML = ''; });
  btnReset.addEventListener('click', resetVM);
  replForm.addEventListener('submit', (event) => {
    event.preventDefault();
    handleReplSubmit();
  });

  btnCopyCode.addEventListener('click', () => {
    navigator.clipboard.writeText(editor.value).then(() => {
      const origText = btnCopyCode.textContent;
      btnCopyCode.textContent = 'Copied!';
      setTimeout(() => { btnCopyCode.textContent = origText; }, 1500);
    });
  });

  exampleSelect.addEventListener('change', (e) => {
    const key = e.target.value;
    if (EXAMPLES[key]) {
      editor.value = EXAMPLES[key];
      updateEditor();
    }
  });

  // Editor keyboard events (Tab, Autoindent, Run Shortcut)
  editor.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
      e.preventDefault();
      handleEditorRun();
      return;
    }

    if (e.key === 'Tab') {
      e.preventDefault();
      const start = editor.selectionStart;
      const end = editor.selectionEnd;
      if (!e.shiftKey) {
        editor.setRangeText('  ', start, end, 'end');
      } else {
        // Dedent
        const lineStart = editor.value.lastIndexOf('\n', start - 1) + 1;
        if (editor.value.substring(lineStart, lineStart + 2) === '  ') {
          editor.setRangeText('', lineStart, lineStart + 2, 'end');
        }
      }
      updateEditor();
      return;
    }

    if (e.key === 'Enter') {
      const pos = editor.selectionStart;
      const lineStart = editor.value.lastIndexOf('\n', pos - 1) + 1;
      const currentLine = editor.value.substring(lineStart, pos);
      const indentMatch = currentLine.match(/^(\s+)/);
      if (indentMatch) {
        e.preventDefault();
        const indent = indentMatch[1];
        editor.setRangeText('\n' + indent, pos, pos, 'end');
        updateEditor();
      }
    }
  });

  editor.addEventListener('input', updateEditor);
  editor.addEventListener('click', updateEditor);
  editor.addEventListener('keyup', updateEditor);
  editor.addEventListener('scroll', syncScroll);

  // REPL keyboard events (Enter, History navigation)
  replInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleReplSubmit();
      return;
    }

    if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (replHistory.length > 0 && historyIndex > 0) {
        historyIndex--;
        replInput.value = replHistory[historyIndex];
      }
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (historyIndex < replHistory.length - 1) {
        historyIndex++;
        replInput.value = replHistory[historyIndex];
      } else {
        historyIndex = replHistory.length;
        replInput.value = '';
      }
    }
  });

  // Split-pane resizer
  let isDragging = false;

  function isStackedLayout() {
    return window.matchMedia('(max-width: 760px)').matches;
  }

  function setEditorPaneSize(clientX, clientY) {
    const rect = workspace.getBoundingClientRect();
    const stacked = isStackedLayout();
    const offset = stacked ? clientY - rect.top : clientX - rect.left;
    const total = stacked ? rect.height : rect.width;
    const percentage = Math.max(20, Math.min(80, (offset / total) * 100));
    editorPane.style.flex = `0 0 ${percentage}%`;
  }

  resizer.addEventListener('pointerdown', (event) => {
    isDragging = true;
    resizer.setPointerCapture(event.pointerId);
    resizer.classList.add('dragging');
    document.body.style.cursor = isStackedLayout() ? 'row-resize' : 'col-resize';
    document.body.style.userSelect = 'none';
  });

  resizer.addEventListener('pointermove', (event) => {
    if (!isDragging) return;
    setEditorPaneSize(event.clientX, event.clientY);
  });

  function stopResizing() {
    if (!isDragging) return;
    isDragging = false;
    resizer.classList.remove('dragging');
    document.body.style.cursor = '';
    document.body.style.userSelect = '';
  }

  resizer.addEventListener('pointerup', stopResizing);
  resizer.addEventListener('pointercancel', stopResizing);
  resizer.addEventListener('keydown', (event) => {
    const stacked = isStackedLayout();
    const previousKey = stacked ? 'ArrowUp' : 'ArrowLeft';
    const nextKey = stacked ? 'ArrowDown' : 'ArrowRight';
    if (event.key !== previousKey && event.key !== nextKey) return;
    event.preventDefault();
    const rect = editorPane.getBoundingClientRect();
    const workspaceRect = workspace.getBoundingClientRect();
    const current = stacked ? rect.height / workspaceRect.height : rect.width / workspaceRect.width;
    const delta = event.key === previousKey ? -0.05 : 0.05;
    editorPane.style.flex = `0 0 ${Math.max(0.2, Math.min(0.8, current + delta)) * 100}%`;
  });

  function updateResizerOrientation() {
    resizer.setAttribute('aria-orientation', isStackedLayout() ? 'horizontal' : 'vertical');
  }

  window.addEventListener('resize', updateResizerOrientation);
  updateResizerOrientation();

  // Initial setup & Query parameter handling (e.g. from documentation "Run in REPL")
  const urlParams = new URLSearchParams(window.location.search);
  const codeParam = urlParams.get('code');
  if (codeParam) {
    editor.value = codeParam;
    updateEditor();
    const runWhenReady = () => {
      setTimeout(() => {
        handleEditorRun();
      }, 300);
    };
    if (isRuntimeReady) {
      runWhenReady();
    } else {
      const origOnReady = window.Module.onRuntimeInitialized;
      window.Module.onRuntimeInitialized = function () {
        if (origOnReady) origOnReady();
        runWhenReady();
      };
    }
  } else {
    editor.value = EXAMPLES.welcome;
    updateEditor();
  }
})();
