set makeprg=./build
autocmd! BufWritePost
autocmd BufWritePost src/* make!
map <M-m> :wall<cr>:make<cr>
