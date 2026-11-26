USE people
INDEX ON age TAG A
GO 2
SCATTER TO ARRAY f
? TRIM(f[1]), f[2]
SCATTER MEMVAR
? TRIM(m.name), m.age
m.age = 77
GATHER MEMVAR
? age
SEEK 77
? FOUND(), RECNO(), TRIM(name)
SEEK 25
? FOUND()
GO 3
SCATTER FIELDS name, age TO ARRAY g
? ALEN(g), TRIM(g[1]), g[2]
g[2] = 55
GATHER FROM ARRAY g FIELDS name, age
? age
CLOSE ALL
