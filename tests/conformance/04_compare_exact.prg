? "abc" = "abc ", "abc" == "abc ", "abc" != "ab"
SET EXACT ON
? "abc" = "abc ", "abc" == "abc "
SET EXACT OFF
? "abc" < "abd", "b" > "a", 1 < 2, 2 >= 2
? .T. .AND. .F., .T. .OR. .F., .NOT. .F.
? .F. .AND. NOSUCHFUNC()
? .T. .OR. NOSUCHFUNC()
