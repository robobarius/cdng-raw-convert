-- cDNG Raw Convert Droplet: drag-and-drop front end for dng2prr.py (fpSup proof of concept)
-- Built into an app by "Build cDNG Raw Convert Droplet.command". Experimental; keep your DNGs.

property appTitle : "cDNG Raw Convert Droplet"
property prefDomain : "org.fpsup.dng2prr"

on run
	set msg to "Drop SIGMA fp clip folders (folders of DNG frames), a folder of clips or a whole card onto this app's icon to convert each clip to an Apple ProRes RAW .mov." & return & return & "Each .mov is saved next to its clip folder." & return & return & my settingsSummary()
	set choice to button returned of (display dialog msg buttons {"Quit", "Settings…", "Choose Folders…"} default button "Choose Folders…" cancel button "Quit" with title appTitle)
	if choice is "Settings…" then
		my editSettings()
	else
		set theItems to choose folder with prompt "Choose clip folders, a folder of clips, or a card:" with multiple selections allowed
		my processItems(theItems)
	end if
end run

on open droppedItems
	my processItems(droppedItems)
end open

-- ------------------------------------------------------------------ settings
on readPref(k, dflt)
	try
		return do shell script "defaults read " & prefDomain & " " & k
	on error
		return dflt
	end try
end readPref

on writePref(k, v)
	do shell script "defaults write " & prefDomain & " " & k & " -string " & quoted form of v
end writePref

on qualityName(s)
	if s is "1" then return "Near-lossless"
	if s is "2" then return "High"
	if s is "4" then return "Standard"
	return "Compact"
end qualityName

on settingsSummary()
	set s to my readPref("scale", "1")
	set n to my readPref("neutral", "")
	if n is "" then
		set wbText to "as recorded by the camera"
	else
		set wbText to "override R/G,B/G = " & n
	end if
	return "Current settings:" & return & "  Quality: " & my qualityName(s) & return & "  White balance: " & wbText
end settingsSummary

on editSettings()
	set opts to {"Near-lossless (largest files, default)", "High (about 20% smaller)", "Standard (about 35% smaller, like a Ninja V 'HQ' recording)", "Compact (about 50% smaller)"}
	set cur to my readPref("scale", "1")
	if cur is "2" then
		set curItem to item 2 of opts
	else if cur is "4" then
		set curItem to item 3 of opts
	else if cur is "8" then
		set curItem to item 4 of opts
	else
		set curItem to item 1 of opts
	end if
	set pick to choose from list opts with title appTitle with prompt "Quality:" default items {curItem}
	if pick is not false then
		set p to item 1 of pick
		if p starts with "High" then
			my writePref("scale", "2")
		else if p starts with "Standard" then
			my writePref("scale", "4")
		else if p starts with "Compact" then
			my writePref("scale", "8")
		else
			my writePref("scale", "1")
		end if
	end if
	set curN to my readPref("neutral", "")
	set ans to display dialog "White balance override (optional)." & return & return & "Leave empty to use the white balance the camera recorded. If the camera's white balance was wrong, enter R/G,B/G measured from a grey card, e.g. 0.737,0.367." default answer curN buttons {"Cancel", "Save"} default button "Save" with title appTitle
	set v to my trim(text returned of ans)
	if v is not "" then
		try
			do shell script "echo " & quoted form of v & " | grep -Eq '^[0-9]*\\.?[0-9]+,[0-9]*\\.?[0-9]+$'"
		on error
			display dialog "That doesn't look like two numbers separated by a comma (e.g. 0.737,0.367). The white balance override was not changed." buttons {"OK"} default button "OK" with icon caution with title appTitle
			return
		end try
	end if
	my writePref("neutral", v)
	display dialog my settingsSummary() buttons {"OK"} default button "OK" with title appTitle
end editSettings

on trim(t)
	return do shell script "printf %s " & quoted form of t & " | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'"
end trim

-- ------------------------------------------------------------------ conversion
on processItems(theItems)
	set res to POSIX path of (path to me) & "Contents/Resources/"
	set py to my findPython(res)
	if py is "" then
		set cmd to "python3 -m pip install --user numpy"
		set b to button returned of (display dialog "This app needs Python 3 with the numpy package, and none was found." & return & return & "1. If macOS has never asked you to install Apple's command-line developer tools, open Terminal and run:  xcode-select --install" & return & "2. Then run this in Terminal:  " & cmd & return & return & "Then drop your clips on the app again." buttons {"Copy Command", "OK"} default button "OK" with icon caution with title appTitle)
		if b is "Copy Command" then set the clipboard to cmd
		return
	end if
	set tool to res & "dng2prr.py"

	set argList to ""
	repeat with anItem in theItems
		set argList to argList & " " & quoted form of (POSIX path of anItem)
	end repeat
	set clipText to do shell script quoted form of py & " " & quoted form of tool & " --list-clips" & argList
	if clipText is "" then
		display dialog "No clip folders with DNG frames were found in what you dropped." buttons {"OK"} default button "OK" with icon caution with title appTitle
		return
	end if
	set clips to paragraphs of clipText

	set scaleOpt to my readPref("scale", "1")
	set neutralOpt to my readPref("neutral", "")
	set opts to " --scale " & scaleOpt & " --progress-every 1"
	if neutralOpt is not "" then set opts to opts & " --neutral " & quoted form of neutralOpt

	set nClips to count of clips
	set doneList to {}
	set failList to {}
	set lastOut to ""
	set progress total steps to 100
	set progress completed steps to 0

	repeat with i from 1 to nClips
		set clipPath to item i of clips
		set clipName to do shell script "basename " & quoted form of clipPath
		set outPath to my outputPathFor(clipPath, clipName)
		set progress description to "Converting clip " & i & " of " & nClips & ": " & clipName
		set progress additional description to "Starting…"
		set progress completed steps to 0
		set logFile to do shell script "mktemp -t dng2prr"
		set pid to do shell script quoted form of py & " -u " & quoted form of tool & " " & quoted form of outPath & " " & quoted form of clipPath & opts & " > " & quoted form of logFile & " 2>&1 & echo $!"
		try
			repeat
				delay 0.5
				set stillGoing to (do shell script "kill -0 " & pid & " 2>/dev/null && echo yes || echo no")
				set prog to do shell script "grep -o 'frame [0-9]*/[0-9]*' " & quoted form of logFile & " | tail -1 || true"
				if prog is not "" then
					set AppleScript's text item delimiters to {" ", "/"}
					set parts to text items of prog
					set AppleScript's text item delimiters to ""
					set fDone to (item 2 of parts) as integer
					set fTotal to (item 3 of parts) as integer
					if fTotal > 0 then set progress completed steps to (fDone * 100 div fTotal)
					set progress additional description to "Frame " & fDone & " of " & fTotal
				end if
				if stillGoing is "no" then exit repeat
			end repeat
		on error number -128
			-- Stop pressed: end the encoder and its worker processes
			do shell script "pkill -TERM -P " & pid & " 2>/dev/null; kill -TERM " & pid & " 2>/dev/null; sleep 1; rm -f " & quoted form of outPath & " || true"
			do shell script "rm -f " & quoted form of logFile
			display dialog "Stopped. Clips finished before stopping were kept." buttons {"OK"} default button "OK" with title appTitle
			return
		end try
		set ok to (do shell script "grep -q '^wrote ' " & quoted form of logFile & " && echo yes || echo no")
		if ok is "yes" then
			set end of doneList to clipName
			set lastOut to outPath
		else
			set errText to do shell script "tail -n 6 " & quoted form of logFile
			set end of failList to clipName & ": " & errText
			do shell script "rm -f " & quoted form of outPath & " || true"
		end if
		do shell script "rm -f " & quoted form of logFile
	end repeat

	set progress completed steps to 100
	set summary to "Converted " & (count of doneList) & " of " & nClips & " clip(s)."
	if (count of failList) > 0 then
		set AppleScript's text item delimiters to return & return
		set summary to summary & return & return & "Problems:" & return & (failList as text)
		set AppleScript's text item delimiters to ""
	end if
	if lastOut is not "" then
		set b to button returned of (display dialog summary buttons {"Done", "Show in Finder"} default button "Show in Finder" with title appTitle)
		if b is "Show in Finder" then do shell script "open -R " & quoted form of lastOut
	else
		display dialog summary buttons {"OK"} default button "OK" with icon caution with title appTitle
	end if
end processItems

-- A Python 3 that can import numpy: the one recorded at build time first, then common locations.
on findPython(res)
	set recorded to ""
	try
		set recorded to do shell script "cat " & quoted form of (res & "python_path")
	end try
	set sh to "for c in " & quoted form of recorded & " /opt/homebrew/bin/python3 /usr/local/bin/python3 /Library/Frameworks/Python.framework/Versions/Current/bin/python3 /usr/bin/python3; do if [ -x \"$c\" ] && \"$c\" -c 'import numpy' >/dev/null 2>&1; then echo \"$c\"; exit 0; fi; done; echo ''"
	return do shell script sh
end findPython

-- Output goes next to the clip folder; falls back to ~/Movies/ProRes RAW if that
-- location isn't writable (e.g. a locked card). Never overwrites an existing file.
on outputPathFor(clipPath, clipName)
	set parentDir to do shell script "dirname " & quoted form of clipPath
	set ok to do shell script "[ -w " & quoted form of parentDir & " ] && echo yes || echo no"
	if ok is "no" then
		set parentDir to (POSIX path of (path to movies folder)) & "ProRes RAW"
		do shell script "mkdir -p " & quoted form of parentDir
	end if
	set base to parentDir & "/" & clipName
	set candidate to base & ".mov"
	set n to 2
	repeat while (do shell script "[ -e " & quoted form of candidate & " ] && echo yes || echo no") is "yes"
		set candidate to base & " (" & n & ").mov"
		set n to n + 1
	end repeat
	return candidate
end outputPathFor
