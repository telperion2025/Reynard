DIMENSION a[4]
a[1] = "one"
a[2] = 2
a[4] = a[2] * 10
? a[1], a[2], a[4], a[3]
? TYPE("a")
DIMENSION m[2, 3]
m[2, 3] = "last"
? m[2, 3], AELEMENT(m, 2, 3), ASUBSCRIPT(m, 6, 1), ASUBSCRIPT(m, 6, 2)
? ALEN(m), ALEN(m, 1), ALEN(m, 2)
ACOPY(a, b)
? b[4]
ASORT(a, 1, 2, 1)
? a[1], a[2]
