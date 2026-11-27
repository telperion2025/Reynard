DEFINE MENU mnu
DEFINE PAD file OF mnu PROMPT "\<File" KEY F
DEFINE PAD rpt OF mnu PROMPT "\<Report"
DEFINE POPUP pop
DEFINE BAR 1 OF pop PROMPT "One"
DEFINE BAR 2 OF pop PROMPT "Two"
ON PAD file OF mnu ? "pad file"
ON SELECTION PAD rpt OF mnu ? "sel rpt"
ON SELECTION BAR 1 OF pop ? "bar one"
ACTIVATE MENU mnu PAD file
SHOW MENU mnu
DEACTIVATE MENU
RELEASE MENU mnu
? "menus done"
