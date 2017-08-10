--TEST--
Decimal128: Special - Negative NaN
--DESCRIPTION--
Generated by scripts/convert-bson-corpus-tests.php

DO NOT EDIT THIS FILE
--FILE--
<?php

require_once __DIR__ . '/../utils/tools.php';

$canonicalBson = hex2bin('18000000136400000000000000000000000000000000FC00');
$canonicalExtJson = '{"d" : {"$numberDecimal" : "NaN"}}';
$degenerateExtJson = '{"d" : {"$numberDecimal" : "-NaN"}}';

// Canonical BSON -> Native -> Canonical BSON 
echo bin2hex(fromPHP(toPHP($canonicalBson))), "\n";

// Canonical BSON -> Canonical extJSON 
echo json_canonicalize(toCanonicalExtendedJSON($canonicalBson)), "\n";

?>
===DONE===
<?php exit(0); ?>
--EXPECT--
18000000136400000000000000000000000000000000fc00
{"d":{"$numberDecimal":"NaN"}}
===DONE===