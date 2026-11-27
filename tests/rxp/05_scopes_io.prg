PUBLIC g
g = "glob"
PRIVATE p
p = "priv"
DO show
? g, p
RELEASE ALL LIKE "p*"
? TYPE("p"), TYPE("g")
WAIT "w"
ACCEPT "a: " TO ans
? ans
INPUT "n: " TO num
? num + 1
PROCEDURE show
? g
ENDPROC
