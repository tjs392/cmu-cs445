SELECT nameFirst || ' (' || nameGiven || ') ' || nameLast AS name, max(HR) as max_hr
FROM appearances a
JOIN people p ON a.playerID = p.playerID
WHERE a.playerID in (
    SELECT cp.playerID
    FROM collegeplaying cp
    JOIN schools s ON cp.schoolID = s.schoolID
    WHERE s.state = 'PA'
)
GROUP BY a.playerID, nameFirst, nameGiven, nameLast
ORDER BY max_hr DESC, nameFirst ASC
LIMIT 10;