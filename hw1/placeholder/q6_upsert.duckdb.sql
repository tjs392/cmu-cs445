INSERT INTO teams (yearID, lgID, teamID, franchID, divID, teamRank, name, attendance)
SELECT yearID, lgID, franchID AS teamID, franchID, divID, teamRank, name, attendance
FROM teams
WHERE yearID = 2024
ON CONFLICT (yearID, lgID, teamID) DO UPDATE
SET attendance = -1;