<img width="1280" height="816" alt="Untitled9_20260407232555" src="https://github.com/user-attachments/assets/e43c6514-cac4-47a2-93a6-92f19863f1bb" />


## VERSION: NAUGHTY BEAR GOLD (Because All DLC Included)


## HOW TO BUILD

Install Rexglue-SDK following the [wiki](https://github.com/rexglue/rexglue-sdk/wiki/Getting-Started)
install Visual Studio Community edition and ensure you install the desktop development with C++ and make sure you check the box that says C++ clang compiler for windows (note: if you are using Mac or linux you can skip this for you will have to follow the wiki linked in step 1 to build the game)
clone/download the repository
dump your copy of Naughty Bear (Xbox360) and use a tool like ISO-Extract to dump the contents of the iso (INCLUDING THE DEFAULT.XEX FILE)
place the contents of the iso in the assets folder (INCLUDING THE DEFAULT.XEX FILE)
open the folder in visual studio, go into cmake targets view
change the configuration to win-amd64-relwithdebinfo
put rexglue.exe in your path environment variable and do rexglue codegen restuffed_config.toml in a terminal (visual studios works, or you can use windows default terminal/cmd/powershell)
right click restuffed project and select build all
copy the assets folder with the dumped contents of the iso into out/build/win-amd64-relwithdebinfo

---

## HOW TO USE

for the time being until a launcher is completed all you must do is go into out/build/win-amd64-relwithdebinfo and either put the assets folder with the dumped assets and the default.xex in it or make a new folder somewhere and place the assets with the default.xex in there
the only folder/files you should have are the game files and the default.xex

---

## CURRENT ISSUES WITH THE GAME

Screen-Tearing
IF YOU FIND ANY CRASHES PLEASE MAKE AN ISSUE EXPLAINING WHERE IT WAS AND WHAT YOU WERE DOING (i.e. game crashed during loading or performing an action on an enemy causing the game to crash)

---

## CREDITS

MadLadMikael - for helping setup and teaching how to use REXGLUESDK and GITHUB

---

## DISCLAIMER

RE-STUFFED AND ITS DEVELOPERS DOES NOT CONDONE PIRACY.
