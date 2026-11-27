DO app.mpr
USE people
ACTIVATE MENU app PAD file
GENERATE SCREEN cust TO custgen.prg
DO custgen.prg
REPORT FORM rep TO FILE out.txt
BEGIN TRANSACTION
GO 1
REPLACE name WITH "Zorch"
END TRANSACTION
? "name=" + name
? "app done"
