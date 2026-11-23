DO greet WITH "bob"
DO addup WITH 1, 2, 3
? fact(5)
? add(3, 4)
s = ""
DO collect WITH "a", "b"
? s
PROCEDURE greet(p)
? "hello", p
ENDPROC
PROCEDURE addup(a, b, c)
? a + b + c
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
PROCEDURE collect(x, y)
PRIVATE s
s = x + y
? "in collect", s
ENDPROC
