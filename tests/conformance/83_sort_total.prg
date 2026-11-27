USE people
SORT TO srt ON age /D
USE srt
LIST OFF FIELDS name, age
CLOSE
USE people
TOTAL TO tot ON married
USE tot
? RECCOUNT()
LIST OFF FIELDS name, age, married
CLOSE ALL
