# md feature tour 🚀

This file shows everything **md** can render. Open it with `md.exe docs\features.md`
to view it, then press `Ctrl+Shift+S` (or start with `md.exe -e`) to edit it beside the preview, which
follows as you type.

## Keyboard shortcuts

| Shortcut | What it does |
| :--- | :--- |
| `F1` | Show / hide a list of all shortcuts |
| `Ctrl+Shift+O` | Switch the split between side by side and stacked |
| `Ctrl+Shift+S` | Split on/off (off shows only the preview, the default) |
| `Ctrl+Shift+X` | Swap which side the editor is on |
| `Ctrl+Shift+E` | Editor only on/off |
| `Ctrl+Shift+Tab` | Move keyboard focus between editor and preview |
| `Ctrl+Shift+N` | Open a file |
| `Ctrl+O` | Open a file (when the preview has focus) |
| `Ctrl+Q` | Quit (when the preview has focus) |
| `Ctrl+A` | Select all text in the preview |
| `Ctrl+C` | Copy the selection as rich text: pasting into Word, Outlook or a browser keeps headings, lists, links, tables and code colours |
| `Ctrl+Shift+C` | Copy the selection as plain text |
| Mouse drag | Select text in the preview; Shift+click extends it |
| `PgUp` / `PgDn` / `Space` / `Home` / `End` | Scroll the preview (when it has focus); the editor follows |
| `j` / `k`, `Ctrl+d` / `Ctrl+u`, `Ctrl+f` / `Ctrl+b`, `gg` / `G` | Vim-style scrolling in the preview; counts work (`5j`, `42G` jumps to source line 42) |
| `/` or `?`, then `n` / `N` | Search the preview forward / backward (Enter keeps the match, Esc cancels), then jump between matches |

Everything else typed in the editor goes to Neovim. Closing the window asks
Neovim to quit, so unsaved changes are never lost silently.

## Headings

# Heading 1
## Heading 2
### Heading 3
#### Heading 4
##### Heading 5
###### Heading 6

Setext heading
--------------

## Inline formatting

Plain text, *italic*, _also italic_, **bold**, __also bold__, ***bold italic***,
~~strikethrough~~, `inline code`, and **bold with `code` inside**.

Escapes: \*not italic\*, \`not code\`, \# not a heading.

Entities: &copy; &amp; &lt;tag&gt; &mdash; &hellip; &#8364; &#x1F600;

A line ending with a backslash\
breaks here, and so does one ending with two spaces  
like this one.

Polish text: Zażółć gęślą jaźń. ZAŻÓŁĆ GĘŚLĄ JAŹŃ.

## Links

- Inline link: [Neovim](https://neovim.io)
- Link with a title: [SDL](https://libsdl.org "Simple DirectMedia Layer")
- Reference link: [tree-sitter][ts] and a collapsed one: [Roboto][]
- Autolink: <https://github.com/soltys/md>
- E-mail: <someone@example.com>
- Relative link to another Markdown file: [README](../README.md)

[ts]: https://tree-sitter.github.io/tree-sitter/
[roboto]: https://fonts.google.com/specimen/Roboto

Click a link in the preview to open it. Links to `.md` files open in md.

## Emoji

Emoji use Noto Color Emoji, in the preview and in the editor:

- Single: 😀 🎉 ❤️ ✅ ⭐ 🔥
- Skin tones: 👍🏻 👍🏽 👍🏿
- Sequences: 👨‍👩‍👧 🧑‍💻 🏳️‍🌈
- Flags: 🇵🇱 🇺🇦 🇪🇺
- Keycaps: 1️⃣ #️⃣

## Lists

- Unordered item
- Another item with a long line that wraps when the preview is narrow, so you can
  see how continuation lines line up
  - Nested item
    - Deeper item
* A different bullet character

1. Ordered item
2. Second
   1. Nested ordered
3. Third

### Task list

- [x] Fast cold start
- [x] Neovim editor
- [ ] Your next feature

## Block quotes

> A quote with **formatting** and a [link](https://example.com).
>
> > A nested quote.

## Code

Inline `code` and fenced blocks with syntax highlighting for C, C++, C#,
Python, JavaScript, JSON, Bash, Rust and Lua:

```c
#include <stdio.h>

/* Comments, keywords, strings, numbers */
int main(void) {
    const char *msg = "hello";
    for (int i = 0; i < 3; ++i) printf("%s %d\n", msg, i);
    return 0;
}
```

```python
def greet(name: str) -> str:
    # f-strings and keywords
    return f"Hello, {name}!" if name else None
```

```javascript
const add = (a, b) => a + b;
console.log(`sum: ${add(2, 3)}`);
```

```json
{ "name": "md", "fast": true, "version": 1 }
```

```bash
cmake --workflow --preset default && echo "done"
```

```rust
fn main() {
    let v: Vec<u32> = (1..=3).collect();
    println!("{:?}", v);
}
```

```lua
local function hello(name)
  return "hi " .. name -- comment
end
```

```cs
public class Hello { static void Main() => System.Console.WriteLine("hi"); }
```

```cpp
template <typename T> T twice(T x) { return x * 2; }
```

    Indented code block
    (four spaces)

## Tables

| Left | Center | Right |
| :--- | :----: | ----: |
| `code` | **bold** | 1 |
| a longer cell that wraps when space runs out | *italic* | 22 |
| 😀 emoji | [link](https://example.com) | 333 |

## Images

Local images (PNG, JPG, GIF, BMP, SVG, QOI, ...) are loaded in the background,
relative to the Markdown file:

![md logo](md-logo.svg)

Remote images are not downloaded; their alt text is shown instead:

![remote image](https://example.com/image.png)

## Horizontal rule

---

## HTML

<details>
<summary>HTML blocks are shown as source</summary>
</details>

That's everything. 🎉
