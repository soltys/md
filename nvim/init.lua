-- md editor config for Neovim 0.12+
--
-- Kept deliberately small: md starts Neovim for every editor pane, so this
-- file is on the cold-start path. Plugins are handled by the built-in
-- vim.pack (no plugin manager to bootstrap).
--
-- Run it isolated from your own config with NVIM_APPNAME, e.g.:
--   set NVIM_APPNAME=md-nvim & nvim -u nvim\init.lua file.md

-- Options ------------------------------------------------------------------
vim.o.termguicolors = true
vim.o.background = "dark"
vim.o.number = true
vim.o.cursorline = true
vim.o.cursorlineopt = "number"  -- highlight only the current line number
vim.o.wrap = true
vim.o.linebreak = true       -- wrap at word boundaries (prose)
vim.o.breakindent = true
vim.o.expandtab = true
vim.o.shiftwidth = 2
vim.o.tabstop = 2
vim.o.swapfile = false       -- md owns the file; avoid swap prompts
vim.o.updatetime = 200       -- quicker CursorHold for preview sync later
vim.o.relativenumber = true  -- with 'number': hybrid line numbers
vim.o.scrolloff = 10         -- lines of context above/below the cursor
vim.o.shiftround = true      -- > and < round the indent to 'shiftwidth'
vim.o.virtualedit = "block"  -- block selection may go past line ends
vim.o.confirm = true         -- ask instead of failing on :q with changes
vim.o.timeoutlen = 300       -- mapped key sequences time out sooner
vim.opt.fillchars:append({ eob = " " })  -- no ~ lines after the end of file
vim.opt.shortmess:append("IWcC")         -- no intro, fewer messages

-- Leader -------------------------------------------------------------------
-- Set before anything maps <leader>, including after/ftplugin files from
-- your own Neovim config (still on 'runtimepath' with -u).
vim.g.mapleader = " "
vim.g.maplocalleader = " "

-- Spell checking -------------------------------------------------------------
-- Markdown buffers only. Turned on after startup so loading the .spl file
-- stays off the first draw. Words added with zg go to the first writable
-- spell/ folder on 'runtimepath' (your Neovim config's spell/en.utf-8.add).
-- Skipped when the en spell file is missing, so Neovim never stops to offer
-- a download inside md.
vim.api.nvim_create_autocmd("FileType", {
  pattern = "markdown",
  callback = function(args)
    vim.schedule(function()
      if not vim.api.nvim_buf_is_valid(args.buf) then return end
      if #vim.api.nvim_get_runtime_file("spell/en.utf-8.spl", false) == 0 then return end
      vim.bo[args.buf].spelllang = "en_us"
      for _, win in ipairs(vim.fn.win_findbuf(args.buf)) do
        vim.api.nvim_set_option_value("spell", true, { scope = "local", win = win })
      end
    end)
  end,
})

-- Clipboard ----------------------------------------------------------------
-- y / d / p use the Windows clipboard, so text moves freely between the
-- editor, the preview (Ctrl+C there) and other programs.
-- Done after startup, so the PATH lookup and the provider stay off the
-- cold-start path; the provider itself loads on first use.
vim.schedule(function()
  if vim.fn.has("win32") == 1 then
    if vim.fn.executable("win32yank.exe") == 1 then
      -- Shipped with the Neovim Windows release: fast, and converts line
      -- endings (CRLF on the clipboard, LF in the buffer).
      vim.g.clipboard = {
        name = "win32yank",
        copy = {
          ["+"] = { "win32yank.exe", "-i", "--crlf" },
          ["*"] = { "win32yank.exe", "-i", "--crlf" },
        },
        paste = {
          ["+"] = { "win32yank.exe", "-o", "--lf" },
          ["*"] = { "win32yank.exe", "-o", "--lf" },
        },
        cache_enabled = 0,
      }
    else
      -- Fallback: PowerShell (slower to start), forced to UTF-8 both ways so
      -- non-ASCII text survives (clip.exe would mangle it).
      local ps = { "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command" }
      local copy = vim.list_extend(vim.deepcopy(ps), {
        "[Console]::InputEncoding = [Text.Encoding]::UTF8;"
          .. " Set-Clipboard -Value ([Console]::In.ReadToEnd())",
      })
      local paste = vim.list_extend(vim.deepcopy(ps), {
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8;"
          .. " $t = Get-Clipboard -Raw; if ($t) { [Console]::Out.Write($t.Replace([string][char]13, '')) }",
      })
      vim.g.clipboard = {
        name = "powershell",
        copy = { ["+"] = copy, ["*"] = copy },
        paste = { ["+"] = paste, ["*"] = paste },
        cache_enabled = 0,
      }
    end
  end
  vim.o.clipboard = "unnamedplus"
end)

-- Plugins ------------------------------------------------------------------
-- First run clones catppuccin into stdpath("data")/site/pack/core/opt.
-- Later runs only add it to 'runtimepath' (no network).
vim.pack.add({
  { src = "https://github.com/catppuccin/nvim", name = "catppuccin" },
  -- Only put on 'runtimepath' here; each is set up on first use below.
  { src = "https://github.com/nvim-mini/mini.nvim", name = "mini.nvim" },
  { src = "https://github.com/folke/flash.nvim", name = "flash.nvim" },
}, { confirm = false })

-- Colorscheme ---------------------------------------------------------------
local ok, catppuccin = pcall(require, "catppuccin")
if ok then
  catppuccin.setup({
    flavour = "mocha",
    -- Stay on Mocha even if the UI/terminal reports a light background.
    background = { light = "mocha", dark = "mocha" },
    integrations = { flash = true, mini = { enabled = true } },
    highlight_overrides = {
      frappe = function(frappe)
        return {
          LineNr = { fg = frappe.text },
          CursorLineNr = { fg = frappe.red },
        }
      end,
      mocha = function(mocha)
        return {
          LineNr = { fg = mocha.text },
          CursorLineNr = { fg = mocha.red },
        }
      end,
    },
  })
  vim.cmd.colorscheme("catppuccin-mocha")
else
  -- Offline on first run: fall back to the built-in scheme.
  vim.cmd.colorscheme("default")
end

-- Lazy-loaded plugins -------------------------------------------------------
-- Nothing below is required at startup. Each plugin is set up the first time
-- one of its keys (or the event it needs) is used.

-- keys = { { modes, lhs }, ... }. Each key starts as a stub that removes all
-- the stubs, runs setup() (which maps the real keys) and replays the key with
-- its count, so the real mapping handles it.
local function lazy_keys(keys, setup)
  local loaded = false
  for _, k in ipairs(keys) do
    vim.keymap.set(k[1], k[2], function()
      local count = vim.v.count > 0 and tostring(vim.v.count) or ""
      if not loaded then
        loaded = true
        for _, s in ipairs(keys) do
          for _, m in ipairs(type(s[1]) == "table" and s[1] or { s[1] }) do
            pcall(vim.keymap.del, m, s[2])
          end
        end
        setup()
      end
      vim.api.nvim_feedkeys(count .. vim.keycode(k[2]), "mi", false)
    end)
  end
end

-- mini.surround: gsa add, gsd delete, gsr replace, gsf/gsF find,
-- gsh highlight, gsn update n_lines (same keys as your own config).
lazy_keys({
  { { "n", "x" }, "gsa" },
  { "n", "gsd" }, { "n", "gsr" }, { "n", "gsf" }, { "n", "gsF" },
  { "n", "gsh" }, { "n", "gsn" },
}, function()
  require("mini.surround").setup({
    mappings = {
      add = "gsa", delete = "gsd", find = "gsf", find_left = "gsF",
      highlight = "gsh", replace = "gsr", update_n_lines = "gsn",
    },
  })
end)

-- mini.move: Alt+h/j/k/l moves the line (or the selection).
lazy_keys({
  { { "n", "x" }, "<M-h>" }, { { "n", "x" }, "<M-j>" },
  { { "n", "x" }, "<M-k>" }, { { "n", "x" }, "<M-l>" },
}, function() require("mini.move").setup({}) end)

-- mini.pairs: from the first time Insert mode is entered.
vim.api.nvim_create_autocmd("InsertEnter", {
  once = true,
  callback = function()
    require("mini.pairs").setup({
      modes = { insert = true, command = true, terminal = false },
      skip_next = [=[[%w%%%'%[%"%.%`%$]]=],  -- not before these characters
      skip_unbalanced = true,
      markdown = true,                      -- handles ``` code fences
    })
  end,
})

-- mini.ai: better a/i text objects (va), yinq, ci', ...), set up the first
-- time Operator-pending or Visual mode starts, before the a/i key is read.
-- Your config's tree-sitter objects (f, c, o, x) are left out.
vim.api.nvim_create_autocmd("ModeChanged", {
  once = true,
  pattern = { "*:no*", "*:v", "*:V", "*:\22" },
  callback = function()
    require("mini.ai").setup({
      n_lines = 500,
      custom_textobjects = {
        t = { "<([%p%w]-)%f[^<%w][^<>]->.-</%1>", "^<.->().*()</[^/]->$" }, -- tags
        d = { "%f[%d]%d+" },                                                -- digits
        e = {                                                               -- CamelCase / snake_case word
          {
            "%u[%l%d]+%f[^%l%d]",
            "%f[%S][%l%d]+%f[^%l%d]",
            "%f[%P][%l%d]+%f[^%l%d]",
            "^[%l%d]+%f[^%l%d]",
          },
          "^().*()$",
        },
        g = function(ai_type)                                               -- whole buffer
          local first, last = 1, vim.fn.line("$")
          if ai_type == "i" then
            first, last = vim.fn.nextnonblank(first), vim.fn.prevnonblank(last)
            if first == 0 or last == 0 then return { from = { line = 1, col = 1 } } end
          end
          local to_col = math.max(vim.fn.getline(last):len(), 1)
          return { from = { line = first, col = 1 }, to = { line = last, col = to_col } }
        end,
      },
    })
  end,
})

-- flash.nvim: s jumps to a label, S selects a tree-sitter node. require()
-- runs on the first press only.
vim.keymap.set({ "n", "x", "o" }, "s", function() require("flash").jump() end, { desc = "Flash" })
vim.keymap.set({ "n", "x", "o" }, "S", function() require("flash").treesitter() end, { desc = "Flash Treesitter" })
