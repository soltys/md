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
vim.o.number = true
vim.o.wrap = true
vim.o.linebreak = true       -- wrap at word boundaries (prose)
vim.o.breakindent = true
vim.o.expandtab = true
vim.o.shiftwidth = 2
vim.o.tabstop = 2
vim.o.swapfile = false       -- md owns the file; avoid swap prompts
vim.o.updatetime = 200       -- quicker CursorHold for preview sync later

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
    flavour = "auto",                          -- follows 'background'
    background = { light = "latte", dark = "mocha" },
    integrations = {},                         -- no other plugins to theme
  })
  vim.cmd.colorscheme("catppuccin")
else
  -- Offline on first run: fall back to the built-in scheme.
  vim.cmd.colorscheme("default")
end
