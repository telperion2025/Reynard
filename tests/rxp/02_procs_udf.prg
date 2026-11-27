DO greet WITH "bob"
? fact(5)
? add(3, 4)
PROCEDURE greet(p)
? "hello", p
ENDPROC
FUNCTION fact(n)
IF n <= 1
RETURN 1
ENDIF
RETURN n * fact(n - 1)
ENDFUNC
FUNCTION add(a, b)
RETURN a + b
ENDFUNC
