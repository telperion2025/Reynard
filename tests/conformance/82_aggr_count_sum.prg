USE people
COUNT
COUNT FOR age > 30 TO c
? c
COUNT NEXT 2 TO c2
? c2
SUM age TO s
? s
AVERAGE age TO av
? av
SUM score TO s2
? s2
CALCULATE CNT(), SUM(age), AVG(age), MIN(age), MAX(age) TO ARRAY c
? c[1], c[2], c[3], c[4], c[5]
CALCULATE FOR married SUM(score) TO sv
? sv
CLOSE ALL
