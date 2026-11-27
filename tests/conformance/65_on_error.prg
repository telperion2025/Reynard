ON ERROR DO handler
? 1/0
? "resumed"
ON ERROR
? "done"
PROCEDURE handler
? "caught", ERROR(), MESSAGE()
ENDPROC
