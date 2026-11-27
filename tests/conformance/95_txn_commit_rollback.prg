USE people
GO 1
? "orig=" + name
BEGIN TRANSACTION
REPLACE name WITH "Temp"
? "inside=" + name
ROLLBACK
? "after rollback=" + name
BEGIN TRANSACTION
REPLACE name WITH "Kept"
END TRANSACTION
? "after commit=" + name
