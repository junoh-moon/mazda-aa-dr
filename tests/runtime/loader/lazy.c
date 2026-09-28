extern int mx5_fixture_missing_function(void);
int fixture_never_called(void) { return mx5_fixture_missing_function(); }
int fixture_answer(void) { return 42; }
