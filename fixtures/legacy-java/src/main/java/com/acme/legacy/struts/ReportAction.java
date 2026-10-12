package com.acme.legacy.struts;

import com.acme.legacy.service.ReportService;
import com.opensymphony.xwork2.ActionSupport;

public class ReportAction extends ActionSupport {
    private final ReportService reports = new ReportService();

    @Override
    public String execute() {
        reports.export("csv");
        return SUCCESS;
    }

    public String list() {
        reports.list();
        return SUCCESS;
    }
}
