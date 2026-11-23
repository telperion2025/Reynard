USE memo
GO 2
? LEN(notes)
REPLACE notes WITH "hello from M2"
CLOSE
USE memo
GO 2
? notes
USE people
COPY TO p SDF
USE blank
APPEND FROM p SDF
? RECCOUNT()
GO TOP
LOCATE FOR age > 30
? FOUND(), RECNO(), TRIM(name)
CONTINUE
? FOUND(), RECNO(), TRIM(name)
