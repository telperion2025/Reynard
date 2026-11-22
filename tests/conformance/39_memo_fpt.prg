USE memo
? RECCOUNT(), FCOUNT(), FIELD(2)
GO 1
? id, notes
GO 2
? LEN(notes)
GO 3
? DELETED(), notes == ""
REPLACE notes WITH "brand new memo written by reynard at M2"
CLOSE
USE memo
GO 3
? notes
