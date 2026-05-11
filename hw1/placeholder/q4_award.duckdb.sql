WITH player_award_winners AS (
    SELECT a.teamID, ap.yearID, COUNT(DISTINCT ap.playerID) as num_winners
    FROM awardsplayers ap
    JOIN appearances a 
        ON a.playerID = ap.playerID
        AND a.yearID = ap.yearID
        AND a.lgID = ap.lgID
    GROUP BY a.teamID, ap.yearID
    HAVING COUNT(DISTINCT ap.playerID) > 5
),
manager_award_years AS (
    SELECT m.teamID, m.yearID
    FROM managers m
    JOIN awardsmanagers am
        ON m.playerID = am.playerID
        AND m.yearID = am.yearID
        AND m.lgID = am.lgID
    GROUP BY m.teamID, m.yearID
)

SELECT lg.league, t.name, COUNT(DISTINCT pa.yearID) AS distinct_years
FROM player_award_winners pa
JOIN manager_award_years ma
    ON pa.teamID = ma.teamID
    AND pa.yearID = ma.yearID
JOIN teams t
    ON t.teamID = pa.teamID
    AND t.yearID = pa.yearID
JOIN leagues lg 
    ON lg.lgID = t.lgID
WHERE lg.active = 'Y'
GROUP BY lg.league, t.name
HAVING COUNT(DISTINCT pa.yearID) > 1
ORDER BY distinct_years DESC, t.name ASC;

