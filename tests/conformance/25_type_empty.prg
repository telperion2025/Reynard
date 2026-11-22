a = "x"
b = 12
c = .T.
d = DATE()
? TYPE(a), TYPE(b), TYPE(c), TYPE(d), VALTYPE(a)
? EMPTY(a), EMPTY(""), EMPTY(0), EMPTY(.F.), EMPTY({})
? ISBLANK(""), ISBLANK("  "), ISBLANK("x")
? INLIST("b", "a", "b", "c"), INLIST(2, 1, 2, 3)
