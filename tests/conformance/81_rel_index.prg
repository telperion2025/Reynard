SELECT 1
USE people
INDEX ON age TAG A
SELECT 2
USE people30 ALIAS kids
SET RELATION TO age INTO people
GO 3
? RECNO(), TRIM(kids->name), TRIM(people->name), people->age
SKIP
? TRIM(kids->name), TRIM(people->name)
? RELATION(1), TARGET(1)
CLOSE ALL
