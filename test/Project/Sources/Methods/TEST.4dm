//%attributes = {}
  //most easy
$connect:="user=miyako dbname=postgres"
$SQL:="SELECT * FROM users"

$params:=New collection:C1472("ccc")
$status:=PQ EXECUTE($connect;$SQL;$params)

