PUBLIC g
g = "glob"
p = "top"
DO sub
? g, p
RELEASE p
? TYPE("p")
RELEASE ALL LIKE "g*"
? TYPE("g")
PROCEDURE sub
PRIVATE p
p = "inner"
? "in", g, p
ENDPROC
