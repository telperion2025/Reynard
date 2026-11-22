? LEN("hello"), UPPER("abc"), LOWER("AbC")
? TRIM("  pad  "), ALLTRIM(" a b "), LTRIM("  x"), RTRIM("x  ")
? LEFT("hello", 2), RIGHT("hello", 2), SUBSTR("hello", 2, 3)
? AT("l", "hello"), ATC("L", "hello"), RAT("l", "hello"), OCCURS("l", "hello")
? "foo" + "bar", "foo  " - "bar"
? REPLICATE("ab", 3), SPACE(3) + "x"
? PADL(42, 6, "*"), PADR("hi", 5, "."), PADC("x", 5, "-")
? STRTRAN("banana", "a", "o"), STUFF("hello", 2, 3, "ELL")
? LIKE("h*o", "hello"), "ell" $ "hello", "xyz" $ "hello"
? PROPER("hello world"), CHR(65), ASC("A")
