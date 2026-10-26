// modPhotoTest - test helper for the magic camera / photo painting mod.
//
// Console command:   givepaintings        -> adds the first painting item found in the item definitions
//                    givepaintings( 3 )   -> adds the first three
//
// "Painting" is a vanilla item tag. Items carrying it open the full-screen painting popup when
// examined in the inventory, which is what the texture finder is used on.

exec function givepaintings( optional howMany : int )
{
	var defMgr    : CDefinitionsManagerAccessor;
	var paintings : array< name >;
	var i, count  : int;

	defMgr = theGame.GetDefinitionsManager();
	paintings = defMgr.GetItemsWithTag( 'Painting' );

	LogChannel( 'modPhotoTest', "Painting items found: " + IntToString( paintings.Size() ) );

	if( paintings.Size() == 0 )
	{
		theGame.GetGuiManager().ShowNotification( "modPhotoTest: no painting items found in the item definitions." );
		return;
	}

	count = howMany;
	if( count <= 0 )
	{
		count = 1;
	}
	if( count > paintings.Size() )
	{
		count = paintings.Size();
	}

	for( i = 0; i < count; i += 1 )
	{
		thePlayer.inv.AddAnItem( paintings[i], 1 );
		LogChannel( 'modPhotoTest', "Added painting item: " + paintings[i] );
	}

	theGame.GetGuiManager().ShowNotification( "modPhotoTest: added " + IntToString( count ) + " painting item(s), first: " + paintings[0] );
}
