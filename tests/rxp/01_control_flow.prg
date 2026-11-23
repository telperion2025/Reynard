x = 7
IF x > 5
? "big"
ELSEIF x > 3
? "mid"
ELSE
? "small"
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
DO CASE
CASE x = 1
? "one"
CASE x = 7
? "seven"
OTHERWISE
? "other"
ENDCASE
