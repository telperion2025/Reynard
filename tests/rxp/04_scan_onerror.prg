USE people
SCAN FOR age >= 30
? TRIM(name), age
ENDSCAN
ON ERROR DO handler
? 1/0
? "recovered"
ON ERROR
? "done"
PROCEDURE handler
? "caught", ERROR(), MESSAGE()
ENDPROC
