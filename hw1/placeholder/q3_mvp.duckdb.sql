


SELECT p.nameGiven, a.teamID, COUNT(DISTINCT ap.yearID) AS distinct_years
FROM appearances a
JOIN awardsplayers ap
    ON ap.playerID = a.playerID
    AND ap.yearID = a.yearID
    AND ap.lgID = a.lgID
JOIN people p on p.playerID = a.playerID
JOIN leagues lg on lg.lgID = ap.lgID
WHERE ap.awardID = 'Gold Glove'
    AND ap.yearID > 1999
    AND lg.active = 'Y'
    AND a.G_batting > (
        SELECT AVG(a2.G_batting)
        FROM appearances a2
        WHERE a2.yearID > 1999
            AND a2.teamID = a.teamID
    )
GROUP BY p.playerID, p.nameGiven, a.teamID
ORDER BY distinct_years DESC, p.nameGiven ASC
LIMIT 10


