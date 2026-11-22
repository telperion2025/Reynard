USE people
? RECCOUNT()
APPEND BLANK
REPLACE name WITH "Zed", age WITH 33, born WITH {^1990-05-06}, married WITH .T., score WITH 55.5
GO BOTTOM
? RECNO(), TRIM(name), age, score, born, married
LIST
