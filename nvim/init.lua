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
}, { confirm = false })

-- Colorscheme ---------------------------------------------------------------
local ok, catppuccin = pcall(require, "catppuccin")
if ok then
  catppuccin.setup({
    flavour = "mocha",
    -- Stay on Mocha even if the UI/terminal reports a light background.
    background = { light = "mocha", dark = "mocha" },
    integrations = {},                         -- no other plugins to theme
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
