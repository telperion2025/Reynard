USE people
LIST FOR age > 30 FIELDS name, age
GO TOP
LIST WHILE age < 40 FIELDS name, age
? RECNO()
DELETE FOR age < 26
LIST FIELDS name, age
