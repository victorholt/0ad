/*
	DESCRIPTION	: Functions for "main game" session GUI.
	NOTES		: 
*/

// ====================================================================

function initSession()
{
	// ============================================= CONSTANTS =================================================

	snConst = new Object();

	// Portraits (large and small).
	snConst.Portrait = new Object();
	snConst.Portrait.Sml = new Object();
	snConst.Portrait.Sml.Width = 36;
	snConst.Portrait.Sml.Height = snConst.Portrait.Sml.Width;
	snConst.Portrait.Lrg = new Object();
	snConst.Portrait.Lrg.Width = 64;
	snConst.Portrait.Lrg.Height = snConst.Portrait.Lrg.Width;

	// Small icons (eg Movement Rate, Food).
	snConst.MiniIcon = new Object();
	snConst.MiniIcon.Width = 20;
	snConst.MiniIcon.Height = snConst.MiniIcon.Width;

	// ============================================= GLOBALS =================================================

	// Define cell reference constants for icon sheets.
	initCellReference();
}

// ====================================================================

function initCellReference()
{
        // Define cell reference constants for icon sheets.

        // icon_sheet_statistic
        stat_accuracy                   = 0;
        stat_attack                     = 1;
        stat_armour                     = 2;
        stat_los                        = 3;
        stat_speed                      = 4;
        stat_range                      = 5;
        stat_hack                       = 6;
        stat_pierce                     = 7;
        stat_crush                      = 8;
        stat_rank1                      = 9;
        stat_rank2                      = 10;
        stat_rank3                      = 11;
        stat_garrison                   = 12;
        stat_heart                      = 13;

        // portrait_sheet_action
                // generic actions
        action_empty                    = 0;
        action_attack                   = 1;
        action_patrol                   = 2;
        action_stop                     = 3;
        action_gather_food              = 4;
        action_gather_wood              = 5;
        action_gather_stone             = 6;
        action_gather_ore               = 7;
        action_rally                    = 8;
        action_repair                   = 9;
        action_heal                     = 10;
        action_scout                    = 11;
        action_townbell                 = 12;
        action_lock                     = 13;
        action_unlock                   = 14;
                // formation actions
        action_formation_box            = 23;
        action_formation_column_c       = 24;
        action_formation_column_o       = 25;
        action_formation_line_c         = 26;
        action_formation_line_o         = 27;
        action_formation_phalanx        = 28;
        action_formation_skirmish       = 29;
        action_formation_testudo        = 30;
        action_formation_wedge          = 31;
                // stance actions
        action_stance_aggress           = 39;
        action_stance_avoid             = 40;
        action_stance_defend            = 41;
        action_stance_hold              = 42;
        action_stance_stand             = 43;
                // tab actions
        action_tab_command              = 48;
        action_tab_train                = 49;
        action_tab_buildciv             = 50;
        action_tab_buildmil             = 51;
        action_tab_research             = 52;
        action_tab_formation            = 53;
        action_tab_stance               = 54;
        action_tab_barter               = 55;
}

// ====================================================================

function setPortrait(objectName, portraitString, portraitSuffix, portraitCell) 
{
        // Use this function as a shortcut to change a portrait object to a different portrait image. 

        // Accepts an object and specifies its default, rollover (lit) and disabled (gray) sprites.
        // Sprite Format: "ui_portrait_"portraitString"_"portraitSuffix
        // Sprite Format: "ui_portrait_"portraitString"_"portraitSuffix"_lit"
        // Sprite Format: "ui_portrait_"portraitString"_"portraitSuffix"_gray"
        // Note: Make sure the file follows this naming convention or bad things could happen.

        // Get GUI object
        setPortraitGUIObject = getGUIObjectByName(objectName);

	// Report error if object not found.
	if (!setPortraitGUIObject)
	{
		console.write ("setPortrait(): Failed to find object " + objectName + ".");
		return 1;
	}

        // Set the three portraits.
	if (portraitSuffix && portraitSuffix != "")
	        setPortraitGUIObject.sprite = "ui_portrait_" + portraitString + "_" + portraitSuffix;
	else
	        setPortraitGUIObject.sprite = "ui_portrait_" + portraitString;

        setPortraitGUIObject.sprite_over = setPortraitGUIObject.sprite + "_lit";
        setPortraitGUIObject.sprite_disabled = setPortraitGUIObject.sprite + "_gray";

        // If the source texture is a multi-frame image (icon sheet), specify correct cell.
        if (portraitCell && portraitCell != "")
                setPortraitGUIObject.cell_id = portraitCell;
	else
		setPortraitGUIObject.cell_id = "";

	return 0;
}

// ====================================================================

function flipGUI (NewGUIType)
{
	// Changes GUI to a different layout.

	switch (NewGUIType)
	{
		// Set which GUI to use.
		case rb:
		case lb:
		case lt:
		case rt:
			GUIType = NewGUIType;
		break;
		default:
			// If no type specified, toggle.
			if (GUIType == rb)
				GUIType = lb;
			else
			if (GUIType == lb)
				GUIType = lt;
			else
			if (GUIType == lt)
				GUIType = rt;
			else
				GUIType = rb;
		break;
	}
	// Seek through all sizes created.
	for (i = 0; i <= Crd.last; i++)
	{
		// Set their sizes to the stored value.
		getGUIObjectByName (Crd[i].name).size = Crd[i].size[GUIType];
	}

	writeConsole("GUI flipped to size " + GUIType + ".");
}

// ====================================================================

// Update-on-alteration trickery...

// We don't really want to update every single time we get a
// selection-changed or property-changed event; that could happen
// a lot. Instead, use this bunch of globals to cache any changes
// that happened between GUI updates.

// This boolean determines whether the selection has been changed.
var selectionChanged = false;

// This boolean determines what the template of the selected object
// was when last we looked
var selectionTemplate = null;

// This array holds the name of all properties that need to be updated
var selectionPropertiesChanged = new Array();

// This array holds a list of all the objects we hold property-change 
// watches on
var propertyWatches = new Array();

// ====================================================================
 
// This function resets all the update variables, above

function resetUpdateVars()
{
	if( selectionChanged )
	{
		for( watchedObject in propertyWatches )
			propertyWatches[watchedObject].unwatchAll( selectionWatchHandler ); // Remove the handler
		
		propertyWatches = new Array();
		if( selection[0] )
		{
			// Watch the object itself
			selection[0].watchAll( selectionWatchHandler );
			propertyWatches.push( selection[0] );
			// And every parent back up the tree (changes there will affect
			// displayed properties via inheritance)
			var parent = selection[0].template
			while( parent )
			{
				parent.watchAll( selectionWatchHandler );
				propertyWatches.push( selection[0] );
				parent = parent.parent;
			}
		}
	}
	selectionChanged = false;
	if( selection[0] ) 
	{
		selectionTemplate = selection[0].template;
	}
	else
		selectionTemplate = null;
		
	selectionPropertiesChanged = new Array();
}

// ====================================================================

// This function returns whether we should update a particular statistic
// in the GUI (e.g. "actions.attack") - this should happen if: the selection
// changed, the selection had its template altered (changing lots of stuff)
// or an assignment has been made to that stat or any property within that
// stat. 

function shouldUpdateStat( statname )
{
	if( selectionChanged || ( selectionTemplate != selection[0].template ) )
		return( true );
	for( var property in selectionPropertiesChanged )
	{
		// If property starts with statname
		if( selectionPropertiesChanged[property].substring( 0, statname.length ) == statname )
			return( true );
	}
	return( false );	
}

// ====================================================================

// This function is a handler for the 'selectionChanged' event,
// it updates the selectionChanged flag

function selectionChangedHandler()
{
	selectionChanged = true;
}

// ====================================================================

// Register it.
addGlobalHandler( "selectionChanged", selectionChangedHandler );

// ====================================================================

// This function is a handler for a watch event; it updates the
// selectionPropertiesChanged array.

function selectionWatchHandler( propname, oldvalue, newvalue )
{
	selectionPropertiesChanged.push( propname );
	// This bit's important (watches allow the handler to change the value
	// before it gets written; we don't want to affect things, so make sure
	// the value we send back is the one that was going to be written)
	return( newvalue ); 
}

// ====================================================================

function snRefresh() 
{
	// Updated each tick to refresh session controls if necessary.

	// Don't process GUI when we're full-screen.
	if (getGUIObjectByName ("sn").hidden == false)
	{
		if (!selection.length)         // If no entity selected,
		{
			// Hide Status Orb
			guiHide ("snStatusPane");

			// Hide Group Pane.
//			guiHide ("snGroupPane");

			getGlobal().MultipleEntitiesSelected = 0;
		}
		else                        // If at least one entity selected,
		{
			// Reveal Status Orb
			guiUnHide ("snStatusPane");

// (later, we need to base this on the selected unit's stats changing)
			refreshStatusPane(); 

			// Check if a group of entities selected
			if (selection.length > 1) 
			{
				// If a group pane isn't already open, and we don't have the same set as last time,
				if (selection.length != getGlobal().MultipleEntitiesSelected)
				{
// (later, we need to base this on the selection changing)
//					refreshGroupPane(); 
					getGlobal().MultipleEntitiesSelected = selection.length;
				}
			} 
			else
			{
				getGlobal().MultipleEntitiesSelected = 0;

				// Hide Group Pane.
//				guiHide ("snGroupPane");
			}
		}

		// Modify any resources given/taken
// (later, we need to base this on a resource-changing event).
//		refreshResourcePool();

		// Update Team Tray
// (later, we need to base this on the player creating a group).
//		refreshTeamTray();
	}
}

// ====================================================================