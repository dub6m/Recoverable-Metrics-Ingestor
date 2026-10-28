import pytest

import lab


@pytest.fixture
def vcan():
    """A private vcan interface for one test. Fails (never skips) if it can't be created."""
    name = lab.unique_name("vtl")
    try:
        lab.vcan_up(name)
    except RuntimeError as error:
        pytest.fail(str(error), pytrace=False)
    yield name
    lab.vcan_down(name)


@pytest.fixture
def logs(tmp_path):
    """Folder for this test's process logs, traces and store files."""
    return tmp_path
