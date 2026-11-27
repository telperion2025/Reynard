USE people
REPORT FORM case TO FILE out.txt
TYPE out.txt
REPORT FORM case TO FILE out2.txt PLAIN HEADING "People" FOR age >= 30 RANGE 1,4
TYPE out2.txt
REPORT FORM case TO FILE out3.txt SUMMARY
TYPE out3.txt
