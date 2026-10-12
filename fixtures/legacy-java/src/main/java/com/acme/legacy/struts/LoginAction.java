package com.acme.legacy.struts;

import com.acme.legacy.service.UserService;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;
import org.apache.struts.action.Action;
import org.apache.struts.action.ActionForm;
import org.apache.struts.action.ActionForward;
import org.apache.struts.action.ActionMapping;

public class LoginAction extends Action {
    @Override
    public ActionForward execute(ActionMapping mapping, ActionForm form, HttpServletRequest req, HttpServletResponse resp) {
        boolean ok = new UserService().authenticate(req.getParameter("user"), req.getParameter("password"));
        return mapping.findForward(ok ? "success" : "failure");
    }
}
