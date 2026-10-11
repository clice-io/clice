void retry(bool again) {
    goto §(done_use)done;
§(done_def)done:
    if (again) {
        goto done;
    }
}
