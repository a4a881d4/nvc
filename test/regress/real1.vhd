entity real1 is
end entity;

architecture test of real1 is
begin

    process is
        variable r : real;
    begin
        assert r = real'left;
        r := 1.0;
        r := r + 1.4;
        assert r > 2.0;
        assert r < 3.0;
        assert r >= real'low;
        assert r <= real'high;
        assert r /= 5.0;
        r := 2.0;
        r := r * 3.0;
        assert r > 5.99999;
        assert r < 6.00001;
        assert integer(r) = 6;
        r := real(5);
        report real'image(r);
        report real'image(-5.262e2);
        report real'image(1.23456);
        report real'image(2.0 ** (-1));
        wait;
    end process;

end architecture;
