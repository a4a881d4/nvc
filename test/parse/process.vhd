architecture a of b is
    signal x : integer := 0;
begin

    p: process is
    begin
    end process;

    process
        variable y : integer := 5;
    begin
        x <= y;
    end process;

    process (x) is
    begin
        x <= x + 1;
    end process;

end architecture;
