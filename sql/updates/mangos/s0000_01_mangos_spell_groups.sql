DROP TABLE IF EXISTS `spell_group`;
CREATE TABLE `spell_group`(
  `Id` mediumint unsigned NOT NULL DEFAULT '0' COMMENT 'Spell Group Identifier',
  `Rule` smallint unsigned NOT NULL,
  `Name` varchar(200) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL COMMENT 'Description of usage',
  PRIMARY KEY(`Id`)
);

DROP TABLE IF EXISTS `spell_group_spell`;
CREATE TABLE `spell_group_spell`(
  `Id` mediumint unsigned NOT NULL DEFAULT '0' COMMENT 'Identifier',
  `SpellId` int unsigned NOT NULL,
  PRIMARY KEY(`Id`,`SpellId`)
);

INSERT INTO spell_group(Id, Rule, Name) VALUES
(1,0,'Food'),
(2,0,'Drink'),
(3,0,'Well Fed'),
(4,0,'Flask Elixir'),
(5,0,'Seal'),
(6,1,'Judgement'),
(7,1,'Blessing'),
(8,1,'Aura'),
(9,1,'Sting'),
(10,1,'Aspect'),
(11,0,'Tracker'),
(12,1,'Curse'),
(13,0,'Mage Armor'),
(14,0,'Warlock Armor'),
(15,0,'Elemental Shield'),
(16,0,'Power Casting Buff'),
(17,0,'Battle Elixir TBC+'),
(18,0,'Guardian Elixir TBC+'),
(19,1,'Shout TBC+'),
(20,1,'Corruption TBC+');

INSERT INTO spell_group_spell(Id, SpellId) VALUES
-- Food
(1,),

-- Drink
(2,),

-- Well Fed
(3,),

-- Flask
-- (4,),

-- Seal
(5,20154), -- Seal of Righteousness
(5,20287),
(5,20288),
(5,20289),
(5,20290),
(5,20291),
(5,20292),
(5,20293),
(5,21084),
(5,27155),
(5,20162), -- Seal of the Crusader
(5,20305),
(5,20306),
(5,20307),
(5,20308),
(5,21082),
(5,27158),
(5,20164), -- Seal of Justice
(5,20165), -- Seal of Light
(5,20347),
(5,20348),
(5,20349),
(5,27160),
(5,20166), -- Seal of Wisdom
(5,20356),
(5,20357),
(5,27166),
(5,20375), -- Seal of Command
(5,20915),
(5,20918),
(5,20919),
(5,20920),
(5,27170),
(5,31892), -- Seal of Blood
(5,31895), -- Seal of Justice

-- Judgement
(6,20184), -- Judgement of Justice
(6,31896),
(6,20185), -- Judgement of Light
(6,20344),
(6,20345),
(6,20346),
(6,27162),
(6,20186), -- Judgement of Wisdom
(6,20354),
(6,20355),
(6,27164),
(6,21183), -- Judgement of the Crusader
(6,20188),
(6,20300),
(6,20301),
(6,20302),
(6,20303),
(6,27159),
(6,20184), -- Judgement of Justice

-- Blessing
(7,1022), -- Blessing of Protection
(7,5599),
(7,10278),
(7,1038), -- Blessing of Salvation
(7,25895), -- Greater Blessing of Salvation
(7,6940), -- Blessing of Sacrifice
(7,20729),
(7,27147),
(7,27148),
(7,19740), -- Blessing of Might
(7,19834),
(7,19835),
(7,19836),
(7,19837),
(7,19838),
(7,25291),
(7,27140),
(7,25782), -- Greater Blessing of Might
(7,25916),
(7,27141),
(7,19742), -- Blessing of Wisdom
(7,19850),
(7,19852),
(7,19853),
(7,19854),
(7,25290),
(7,27142),
(7,25894), -- Greater Blessing of Wisdom
(7,25918),
(7,27143),
(7,19977), -- Blessing of Light
(7,19978),
(7,19979),
(7,27144),
(7,25890), -- Greater Blessing of Light
(7,27145),
(7,20217), -- Blessing of Kings
(7,25898), -- Greater Blessing of Kings
(7,20911), -- Blessing of Sanctuary
(7,20912),
(7,20913),
(7,20914),
(7,27168),
(7,25899), -- Greater Blessing of Sanctuary
(7,27169),

-- Aura
(8,465), -- Devotion Aura
(8,10290),
(8,643),
(8,10291),
(8,1032),
(8,10292),
(8,10293),
(8,27149),
(8,7294), -- Retribution Aura
(8,10298),
(8,10299),
(8,10300),
(8,10301),
(8,27150),
(8,19746), -- Concentration Aura
(8,19876), -- Shadow Resistance Aura
(8,19895),
(8,19896),
(8,27151),
(8,19888), -- Frost Resistance Aura
(8,19897),
(8,19898),
(8,27152),
(8,19891), -- Fire Resistance Aura
(8,19899),
(8,19900),
(8,27153),
(8,20218), -- Sanctity Aura
(8,32223), -- Crusader Aura

-- Sting
(9,1978), -- serpent sting
(9,13549),
(9,13550),
(9,13551),
(9,13552),
(9,13553),
(9,13554),
(9,13555),
(9,25295),
(9,27016),
(9,3034), -- viper sting
(9,14279),
(9,14280),
(9,27018),
(9,3043), -- scorpid sting
(9,19386), -- Wyvern Sting
(9,24131),
(9,24132),
(9,24133),
(9,24134),
(9,24135),
(9,27068),
(9,27069),

-- aspect
(10,13161), -- Aspect of the Beast
(10,5118), -- Aspect of the Cheetah
(10,13159), -- Pack
(10,13163), -- Monkey
(10,13165), -- Hawk
(10,14318),
(10,14319),
(10,14320),
(10,14321),
(10,14322),
(10,25296),
(10,27044),
(10,20043), -- Wild
(10,20190),
(10,27045),
(10,34074), -- Viper

-- Tracker
(11,1494), -- Track Beasts
(11,2383), -- Find Herbs
(11,2481), -- Find Treasure
(11,2580), -- Find Minerals
(11,5225), -- Track Humanoids
(11,5500), -- Sense Demons
(11,5502), -- Sense Undead
(11,11389), -- Detect Undead
(11,11407), -- Detect Demon
(11,19878), -- Track Demons
(11,19879), -- Track Dragonkin
(11,19880), -- Track Elementals
(11,19882), -- Track Giants
(11,19883), -- Track Humanoids
(11,19884), -- Track Undead
(11,19885), -- Track Hidden
(11,30645), -- Gas Cloud Tracking
(11,43308), -- Find Fish

-- Curse
(12,603), -- doom
(12,30910),
(12,702), -- weakness
(12,1108),
(12,6205),
(12,7646),
(12,11707),
(12,11708),
(12,27224),
(12,30909),
(12,704), -- recklessness
(12,7658),
(12,7659),
(12,11717),
(12,27226),
(12,1714), -- tongues
(12,11719),
(12,980), -- agony
(12,1014),
(12,6217),
(12,11711),
(12,11712),
(12,11713),
(12,27218),
(12,1490), -- elements
(12,11721),
(12,11722),
(12,27228),
(12,17862), -- shadow
(12,17937),
(12,27229),

-- Mage Armor
(13,168), -- frost armor
(13,7300),
(13,7301),
(13,7302), -- ice armor
(13,7320),
(13,10219),
(13,10220),
(13,27124),
(13,6117), -- mage armor
(13,22782),
(13,22783),
(13,27125),
(13,30482), -- molten armor

-- Warlock armor
(14,687), -- demon skin
(14,696),
(14,706), -- demon armor
(14,1086),
(14,11733),
(14,11734),
(14,11735),
(14,27260),
(14,28176), -- Fel Armor
(14,28189),

-- Elemental Shield
(15,23552), -- T2 8 bonus
(15,324), -- Lightning Shield
(15,325),
(15,905),
(15,945),
(15,8134),
(15,10431),
(15,10432),
(15,25469),
(15,25472),
(15,24398), -- Water Shield
(15,33736),
(15,974), -- Earth Shield
(15,32593),
(15,32594),

-- Power Casting Buff
(16,12042), -- Arcane Power
(16,10060), -- Power Infusion

-- Battle Elixir
-- (17,),

-- Guardian Elixir
-- (18,),

-- Shout
(19,469), -- commanding shout
(19,6673), -- battle shout
(19,5242),
(19,6192),
(19,11549),
(19,11550),
(19,11551),
(19,25289),
(19,2048),

-- Corruption
(20,27243), -- seed
(20,172), -- corruption
(20,6222),
(20,6223),
(20,7648),
(20,11671),
(20,11672),
(20,25311),
(20,27216);

INSERT INTO spell_group_spell SELECT 17, entry FROM spell_elixir WHERE Mask&0x01 != 0; -- battle elixir
INSERT INTO spell_group_spell SELECT 18, entry FROM spell_elixir WHERE Mask&0x02 != 0; -- guardian elixir
INSERT INTO spell_group_spell SELECT 3, entry FROM spell_elixir WHERE Mask&0x10 != 0; -- well fed


