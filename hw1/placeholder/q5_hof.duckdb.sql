WITH inducted AS (
    SELECT DISTINCT playerID
    FROM halloffame
    WHERE inducted = 'Y'
)
SELECT 
    p.nameFirst || ' (' || p.nameGiven || ') ' || p.nameLast AS hof_player_name,
    teammate.teammate_name AS earliest_teammate_name,
    teammate.teammate_year AS earliest_teammate_year
FROM inducted hof
JOIN people p ON p.playerID = hof.playerID
JOIN LATERAL (
    SELECT 
        tp.nameFirst || ' (' || tp.nameGiven || ') ' || tp.nameLast AS teammate_name,
        a1.yearID AS teammate_year
    FROM appearances a1
    JOIN appearances a2 
        ON  a1.teamID = a2.teamID
        AND a1.yearID = a2.yearID
        AND a1.playerID != a2.playerID
    JOIN people tp ON tp.playerID = a2.playerID
    WHERE a1.playerID = hof.playerID
    ORDER BY a1.yearID ASC, teammate_name ASC
    LIMIT 1
) AS teammate ON TRUE
ORDER BY hof_player_name ASC
LIMIT 10;