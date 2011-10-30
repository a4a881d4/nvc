entity bot is
    generic ( N : integer );
    port ( o : out integer );
end entity;

architecture a of bot is
begin

    process is
    begin
        o <= N;
        wait;
    end process;
    
end architecture;

-------------------------------------------------------------------------------

entity top is
end entity;

architecture test of top is
    signal x : integer;
begin

    bot0: entity work.bot               -- OK
        generic map ( N => 5 )
        port map ( o => x );

    bot1: entity work.bot               -- OK
        generic map ( 5 )
        port map ( o => x );

    bot3: entity work.bot               -- Missing N
        port map ( o => x );

    bot4: entity work.bot               -- Too many generics
        generic map ( 1, 2 )
        port map ( o => x );

    

end architecture;