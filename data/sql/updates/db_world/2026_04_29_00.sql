-- DB update 2026_04_27_00 -> 2026_04_29_00
-- Remove spirit healer spawns for hardcore setup.
DELETE c
FROM `creature` c
JOIN `creature_template` ct ON ct.`entry` = c.`id1`
WHERE (ct.`npcflag` & 16384) != 0;

-- DB update 2026_04_28_06 -> 2026_04_29_00
DELETE FROM `smart_scripts` WHERE `entryorguid`=28615 AND `source_type`=0 AND `id` IN (0,1);
UPDATE `creature_template` SET `AIName`='' WHERE `entry`=28615;
