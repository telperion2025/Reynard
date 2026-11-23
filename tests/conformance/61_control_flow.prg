x = 7
IF x > 5
? "big"
ELSEIF x > 3
? "mid"
ELSE
? "small"
ENDIF
y = 2
IF y > 5
? "big2"
ELSEIF y > 3
? "mid2"
ELSE
? "small2"
ENDIF
i = 0
DO WHILE i < 4
i = i + 1
IF i = 2
LOOP
ENDIF
IF i = 4
EXIT
ENDIF
? "w", i
ENDDO
FOR j = 10 TO 1 STEP -3
? "f", j
NEXT
FOR k = 1 TO 3
? "k", k
NEXT
DO CASE
CASE x = 1
? "one"
CASE x = 7
? "seven"
OTHERWISE
? "other"
ENDCASE
