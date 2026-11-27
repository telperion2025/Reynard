USE people
SCAN FOR age >= 30
? TRIM(name), age
ENDSCAN
? "eof", EOF()
SCAN
IF RECNO() = 2
LOOP
ENDIF
IF RECNO() = 4
EXIT
ENDIF
? "s", RECNO(), TRIM(name)
ENDSCAN
? "after"
SCAN WHILE age < 40
? "w", TRIM(name)
ENDSCAN
